// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2026, Eidel Solomon. All rights reserved.
 */

/*
 * ZFS Rebase — three-way merge at the block level.
 *
 * Given two datasets — left and right — auto-discover their common
 * ancestor snapshot (A), then produce a merged result X' containing
 * the left HEAD's state plus the right side's non-conflicting
 * changes, built in-chain on top of the left HEAD using normal DMU
 * write operations.
 *
 * See the block comment in dsl_rebase.h for type definitions.
 */

#include <sys/zfs_context.h>
#include <sys/dsl_rebase.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_scan.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/nvpair.h>
#include <sys/zap.h>
#include <sys/zfs_znode.h>
#include <sys/sa.h>
#include <sys/zfs_sa.h>
#include <sys/dnode.h>

/*
 * Snapshot chain entry for common-ancestor discovery.
 */
typedef struct rebase_snap_entry {
	uint64_t	rse_guid;
	uint64_t	rse_obj;
	avl_node_t	rse_node;
} rebase_snap_entry_t;

static int
rebase_snap_entry_cmp(const void *a, const void *b)
{
	const rebase_snap_entry_t *la = a;
	const rebase_snap_entry_t *lb = b;

	return (TREE_CMP(la->rse_guid, lb->rse_guid));
}

static void
rebase_snap_tree_destroy(avl_tree_t *tree)
{
	rebase_snap_entry_t *rse;
	void *cookie = NULL;

	while ((rse = avl_destroy_nodes(tree, &cookie)) != NULL)
		kmem_free(rse, sizeof (*rse));
	avl_destroy(tree);
}

/*
 * Build an AVL tree of {guid, obj} pairs for every snapshot reachable
 * from ds by following ds_prev_snap_obj.  The dataset ds itself is
 * NOT included — only its snapshots.
 */
static int
rebase_collect_snap_chain(dsl_pool_t *dp, dsl_dataset_t *ds,
    avl_tree_t *tree)
{
	uint64_t obj;
	int err = 0;

	avl_create(tree, rebase_snap_entry_cmp,
	    sizeof (rebase_snap_entry_t),
	    offsetof(rebase_snap_entry_t, rse_node));

	obj = dsl_dataset_phys(ds)->ds_prev_snap_obj;

	while (obj != 0) {
		dsl_dataset_t *snap;
		rebase_snap_entry_t *rse;

		err = dsl_dataset_hold_obj(dp, obj, FTAG, &snap);
		if (err != 0)
			break;

		rse = kmem_alloc(sizeof (*rse), KM_SLEEP);
		rse->rse_guid = dsl_dataset_phys(snap)->ds_guid;
		rse->rse_obj = obj;
		avl_add(tree, rse);

		obj = dsl_dataset_phys(snap)->ds_prev_snap_obj;
		dsl_dataset_rele(snap, FTAG);
	}

	return (err);
}

/*
 * Find the common ancestor snapshot between two datasets.
 *
 * Strategy: collect the left chain into an AVL tree keyed by guid,
 * then walk the right chain looking for a guid match.  The first
 * match (closest to the heads) is the common ancestor.
 *
 * On success, *base is held and the caller must release it.
 */
static int
rebase_find_common(dsl_pool_t *dp, dsl_dataset_t *left,
    dsl_dataset_t *right, dsl_dataset_t **base)
{
	avl_tree_t left_snaps;
	rebase_snap_entry_t search;
	uint64_t obj;
	int err;

	err = rebase_collect_snap_chain(dp, left, &left_snaps);
	if (err != 0) {
		rebase_snap_tree_destroy(&left_snaps);
		return (err);
	}

	/*
	 * If right is a snapshot that already appears in the left
	 * chain, the history is already linear — nothing to rebase.
	 */
	if (right->ds_is_snapshot) {
		search.rse_guid = dsl_dataset_phys(right)->ds_guid;
		if (avl_find(&left_snaps, &search, NULL) != NULL) {
			rebase_snap_tree_destroy(&left_snaps);
			return (SET_ERROR(EINVAL));
		}
	}

	obj = dsl_dataset_phys(right)->ds_prev_snap_obj;

	while (obj != 0) {
		dsl_dataset_t *snap;

		err = dsl_dataset_hold_obj(dp, obj, FTAG, &snap);
		if (err != 0)
			break;

		search.rse_guid = dsl_dataset_phys(snap)->ds_guid;
		if (avl_find(&left_snaps, &search, NULL) != NULL) {
			*base = snap;
			rebase_snap_tree_destroy(&left_snaps);
			return (0);
		}

		obj = dsl_dataset_phys(snap)->ds_prev_snap_obj;
		dsl_dataset_rele(snap, FTAG);
	}

	rebase_snap_tree_destroy(&left_snaps);
	return (SET_ERROR(ENOENT));
}

/*
 * AVL comparators for rebase_change_t.
 */
static int
rebase_change_path_cmp(const void *a, const void *b)
{
	const rebase_change_t *la = a;
	const rebase_change_t *lb = b;

	return (strcmp(la->rc_path, lb->rc_path));
}

static int
rebase_change_obj_cmp(const void *a, const void *b)
{
	const rebase_change_t *la = a;
	const rebase_change_t *lb = b;
	int cmp;

	cmp = TREE_CMP(la->rc_obj, lb->rc_obj);
	if (cmp != 0)
		return (cmp);
	return (strcmp(la->rc_path, lb->rc_path));
}

static void
rebase_changelist_init(rebase_changelist_t *rcl)
{
	avl_create(&rcl->rcl_by_path, rebase_change_path_cmp,
	    sizeof (rebase_change_t),
	    offsetof(rebase_change_t, rc_avl_path));
	avl_create(&rcl->rcl_by_obj, rebase_change_obj_cmp,
	    sizeof (rebase_change_t),
	    offsetof(rebase_change_t, rc_avl_obj));
	rcl->rcl_count = 0;
}

static void
rebase_changelist_fini(rebase_changelist_t *rcl)
{
	rebase_change_t *rc;
	void *cookie = NULL;

	while ((rc = avl_destroy_nodes(&rcl->rcl_by_path,
	    &cookie)) != NULL) {
		avl_remove(&rcl->rcl_by_obj, rc);
		if (rc->rc_path != NULL)
			kmem_free(rc->rc_path, rc->rc_pathlen);
		if (rc->rc_old_path != NULL)
			kmem_free(rc->rc_old_path, rc->rc_old_pathlen);
		kmem_free(rc, sizeof (*rc));
	}
	avl_destroy(&rcl->rcl_by_path);
	avl_destroy(&rcl->rcl_by_obj);
}

/*
 * Read a uint64 from a dataset's MASTER_NODE ZAP.
 */
static int
rebase_master_lookup(objset_t *os, const char *key, uint64_t *valp)
{
	return (zap_lookup(os, MASTER_NODE_OBJ, key, 8, 1, valp));
}

/*
 * Set up SA attribute tables for an objset.
 * Looks up the SA_ATTRS object from MASTER_NODE, then calls sa_setup().
 * Idempotent — if os->os_sa is already initialized, sa_setup()
 * returns the cached table.
 */
static int
rebase_sa_setup(objset_t *os, sa_attr_type_t **sa_tblp)
{
	uint64_t sa_obj = 0;
	int err;

	err = rebase_master_lookup(os, ZFS_SA_ATTRS, &sa_obj);
	if (err != 0 && err != ENOENT)
		return (err);

	return (sa_setup(os, sa_obj, zfs_attr_table, ZPL_END, sa_tblp));
}

/*
 * Validate preconditions for a rebase operation.
 *
 * Checks (all three datasets are held, objsets opened):
 *   1. Common ancestor exists (already verified by find_common)
 *   2. No active scrub or resilver
 *   3. Encryption compatibility (both encrypted or both not)
 *   4. ZPL version >= 5 on all three
 *   5. FUID table object identical across all three
 *
 * Not checked:
 *   - User holds: rebase only adds history after the left tip,
 *     never rewrites existing snapshots, so holds are harmless.
 *   - Right-side clones/snapshots: rebase only reads from the
 *     right side, never mutates it.  Clones of intermediate
 *     snapshots are unaffected.
 */
static int
rebase_check_preconditions(dsl_pool_t *dp, dsl_dataset_t *left,
    dsl_dataset_t *right, dsl_dataset_t *base, objset_t *left_os,
    objset_t *right_os, objset_t *base_os)
{
	uint64_t left_val, right_val, base_val;
	int err;

	(void) base;

	/* (2) Scrub/resilver in progress — avoid data races. */
	if (dsl_scan_active(dp->dp_scan))
		return (SET_ERROR(EBUSY));

	/* (4) Encryption — both sides must agree. */
	if ((left->ds_dir->dd_crypto_obj == 0) !=
	    (right->ds_dir->dd_crypto_obj == 0))
		return (SET_ERROR(EACCES));

	/* (5) ZPL >= 5 (SA layout required). */
	err = rebase_master_lookup(left_os, ZPL_VERSION_STR, &left_val);
	if (err != 0)
		return (err);
	if (left_val < ZPL_VERSION_SA)
		return (SET_ERROR(ENOTSUP));

	err = rebase_master_lookup(right_os, ZPL_VERSION_STR, &right_val);
	if (err != 0)
		return (err);
	if (right_val < ZPL_VERSION_SA)
		return (SET_ERROR(ENOTSUP));

	err = rebase_master_lookup(base_os, ZPL_VERSION_STR, &base_val);
	if (err != 0)
		return (err);
	if (base_val < ZPL_VERSION_SA)
		return (SET_ERROR(ENOTSUP));

	/* (6) FUID table object must match across all three. */
	left_val = right_val = base_val = 0;
	(void) rebase_master_lookup(left_os, ZFS_FUID_TABLES, &left_val);
	(void) rebase_master_lookup(right_os, ZFS_FUID_TABLES, &right_val);
	(void) rebase_master_lookup(base_os, ZFS_FUID_TABLES, &base_val);

	if (left_val != right_val || left_val != base_val)
		return (SET_ERROR(ENOTSUP));

	return (0);
}

/*
 * Build a child path by appending "/name" to parent.
 * Returns a kmem_alloc'd string; *lenp receives the allocation
 * size (including the NUL terminator).
 */
static char *
rebase_build_path(const char *parent, size_t parentlen,
    const char *name, size_t *lenp)
{
	size_t plen = parentlen - 1;
	size_t nlen = strlen(name);
	size_t alloc;
	char *path;

	if (plen == 1 && parent[0] == '/') {
		alloc = 1 + nlen + 1;
		path = kmem_alloc(alloc, KM_SLEEP);
		path[0] = '/';
		memcpy(path + 1, name, nlen + 1);
	} else {
		alloc = plen + 1 + nlen + 1;
		path = kmem_alloc(alloc, KM_SLEEP);
		memcpy(path, parent, plen);
		path[plen] = '/';
		memcpy(path + plen + 1, name, nlen + 1);
	}

	*lenp = alloc;
	return (path);
}

/*
 * Allocate a change record and insert it into both AVL trees.
 * The path string is copied; the caller retains ownership.
 */
static void
rebase_record_change(rebase_changelist_t *rcl,
    rebase_change_type_t type, uint64_t obj,
    const char *path, size_t pathlen, uint8_t dn_type)
{
	rebase_change_t *rc;

	rc = kmem_zalloc(sizeof (*rc), KM_SLEEP);
	rc->rc_type = type;
	rc->rc_obj = obj;
	rc->rc_pathlen = pathlen;
	rc->rc_path = kmem_alloc(pathlen, KM_SLEEP);
	memcpy(rc->rc_path, path, pathlen);
	rc->rc_dn_type = dn_type;

	avl_add(&rcl->rcl_by_path, rc);
	avl_add(&rcl->rcl_by_obj, rc);
	rcl->rcl_count++;
}

/*
 * Recursively record every entry in a single-side directory tree
 * as type (RCT_ADD or RCT_DELETE).  Used when an entire directory
 * exists on only one side of the diff.
 */
static int
rebase_walk_tree(objset_t *os, rebase_changelist_t *rcl,
    rebase_change_type_t type, uint64_t dir_obj,
    const char *path, size_t pathlen, zap_attribute_t *za)
{
	zap_cursor_t zc;
	int err = 0;

	for (zap_cursor_init(&zc, os, dir_obj);
	    (err = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		uint64_t child_obj = ZFS_DIRENT_OBJ(za->za_first_integer);
		dmu_object_info_t doi;
		char *cpath;
		size_t cpathlen;

		cpath = rebase_build_path(path, pathlen,
		    za->za_name, &cpathlen);

		err = dmu_object_info(os, child_obj, &doi);
		if (err != 0) {
			kmem_free(cpath, cpathlen);
			break;
		}

		rebase_record_change(rcl, type, child_obj,
		    cpath, cpathlen, doi.doi_type);

		if (doi.doi_type == DMU_OT_DIRECTORY_CONTENTS) {
			err = rebase_walk_tree(os, rcl, type,
			    child_obj, cpath, cpathlen, za);
			if (err != 0) {
				kmem_free(cpath, cpathlen);
				break;
			}
		}

		kmem_free(cpath, cpathlen);
	}

	if (err == ENOENT)
		err = 0;

	zap_cursor_fini(&zc);
	return (err);
}

/*
 * Compare a single fixed-size (uint64_t) SA attribute between
 * two handles.  Both-absent counts as equal.
 */
static int
rebase_sa_cmp_uint64(sa_handle_t *hdl_a, sa_handle_t *hdl_b,
    sa_attr_type_t attr, boolean_t *samep)
{
	uint64_t va = 0, vb = 0;
	int ea, eb;

	ea = sa_lookup(hdl_a, attr, &va, sizeof (va));
	eb = sa_lookup(hdl_b, attr, &vb, sizeof (vb));

	if (ea == ENOENT && eb == ENOENT) {
		*samep = B_TRUE;
		return (0);
	}
	if (ea != 0 && ea != ENOENT)
		return (ea);
	if (eb != 0 && eb != ENOENT)
		return (eb);
	if (ea != eb) {
		*samep = B_FALSE;
		return (0);
	}

	*samep = (va == vb);
	return (0);
}

/*
 * Compare a variable-length SA attribute between two handles.
 * Both-absent counts as equal.
 */
static int
rebase_sa_cmp_var(sa_handle_t *hdl_a, sa_handle_t *hdl_b,
    sa_attr_type_t attr, boolean_t *samep)
{
	int sa, sb;
	int ea, eb;
	void *buf_a, *buf_b;

	ea = sa_size(hdl_a, attr, &sa);
	eb = sa_size(hdl_b, attr, &sb);

	if (ea == ENOENT && eb == ENOENT) {
		*samep = B_TRUE;
		return (0);
	}
	if (ea != 0 && ea != ENOENT)
		return (ea);
	if (eb != 0 && eb != ENOENT)
		return (eb);
	if (ea != eb || sa != sb) {
		*samep = B_FALSE;
		return (0);
	}

	buf_a = kmem_alloc(sa, KM_SLEEP);
	buf_b = kmem_alloc(sb, KM_SLEEP);

	ea = sa_lookup(hdl_a, attr, buf_a, sa);
	if (ea != 0) {
		kmem_free(buf_a, sa);
		kmem_free(buf_b, sb);
		return (ea);
	}

	eb = sa_lookup(hdl_b, attr, buf_b, sb);
	if (eb != 0) {
		kmem_free(buf_a, sa);
		kmem_free(buf_b, sb);
		return (eb);
	}

	*samep = (memcmp(buf_a, buf_b, sa) == 0);

	kmem_free(buf_a, sa);
	kmem_free(buf_b, sb);
	return (0);
}

/*
 * SA identity attributes — fields that constitute the "identity"
 * of a file or directory.  Timestamps and ZPL_GEN are excluded:
 * rename-on-save always creates a new generation and updates
 * timestamps, so including them would prevent hysterical detection.
 */
static const sa_attr_type_t rebase_identity_fixed[] = {
	ZPL_MODE, ZPL_UID, ZPL_GID, ZPL_FLAGS,
	ZPL_RDEV, ZPL_PROJID, ZPL_SIZE, ZPL_DACL_COUNT
};

static const sa_attr_type_t rebase_identity_var[] = {
	ZPL_DACL_ACES, ZPL_DXATTR, ZPL_SYMLINK
};

/*
 * Compare SA identity fields between two objects.  Sets *samep
 * to B_TRUE if all identity attributes match.  Opens and closes
 * SA handles internally.
 */
static int
rebase_sa_identity_equal(objset_t *os_a, uint64_t obj_a,
    objset_t *os_b, uint64_t obj_b, boolean_t *samep)
{
	sa_handle_t *hdl_a = NULL, *hdl_b = NULL;
	boolean_t same;
	int err;

	*samep = B_FALSE;

	err = sa_handle_get(os_a, obj_a, NULL,
	    SA_HDL_PRIVATE, &hdl_a);
	if (err != 0)
		return (err);

	err = sa_handle_get(os_b, obj_b, NULL,
	    SA_HDL_PRIVATE, &hdl_b);
	if (err != 0) {
		sa_handle_destroy(hdl_a);
		return (err);
	}

	for (int i = 0;
	    i < sizeof (rebase_identity_fixed) /
	    sizeof (rebase_identity_fixed[0]); i++) {
		err = rebase_sa_cmp_uint64(hdl_a, hdl_b,
		    rebase_identity_fixed[i], &same);
		if (err != 0)
			goto out;
		if (!same)
			goto out;
	}

	for (int i = 0;
	    i < sizeof (rebase_identity_var) /
	    sizeof (rebase_identity_var[0]); i++) {
		err = rebase_sa_cmp_var(hdl_a, hdl_b,
		    rebase_identity_var[i], &same);
		if (err != 0)
			goto out;
		if (!same)
			goto out;
	}

	*samep = B_TRUE;

out:
	sa_handle_destroy(hdl_b);
	sa_handle_destroy(hdl_a);
	return (err);
}

/*
 * Detect hysterical edits — two dnode objects (possibly at the same
 * slot index across objsets, or at different indices within the same
 * objset) whose identity SA fields and file data are identical.
 *
 * Two forms:
 *   1. Different index, same path — rename-on-save editors (nvim,
 *      sed -i) allocate a new dnode with the same content.
 *   2. Same index, same path — file was COW'd (edit + edit-back,
 *      or metadata-only timestamp churn) but data and identity
 *      fields are unchanged from the snapshot.
 *
 * Returns B_TRUE in *hystp when the two objects are content-identical
 * and should NOT be recorded as an EDIT.
 */
static int
rebase_is_hysterical(objset_t *base_os, uint64_t base_obj,
    objset_t *side_os, uint64_t side_obj, boolean_t *hystp)
{
	dmu_object_info_t doi_a, doi_b;
	boolean_t same;
	int err;

	*hystp = B_FALSE;

	err = rebase_sa_identity_equal(base_os, base_obj,
	    side_os, side_obj, &same);
	if (err != 0)
		return (err);
	if (!same)
		return (0);

	/*
	 * Identity fields match — compare file data.
	 *
	 * Fast path: if compression, checksum algorithm, and data
	 * block size all agree, we can compare the top-level block
	 * pointer checksums instead of reading the data.  Matching
	 * 256-bit checksums on identical algorithms means the data
	 * is byte-identical.
	 */
	err = dmu_object_info(base_os, base_obj, &doi_a);
	if (err != 0)
		return (err);
	err = dmu_object_info(side_os, side_obj, &doi_b);
	if (err != 0)
		return (err);

	if (doi_a.doi_max_offset != doi_b.doi_max_offset)
		return (0);

	if (doi_a.doi_compress == doi_b.doi_compress &&
	    doi_a.doi_checksum == doi_b.doi_checksum &&
	    doi_a.doi_data_block_size == doi_b.doi_data_block_size &&
	    doi_a.doi_nblkptr == doi_b.doi_nblkptr) {
		dnode_t *dn_a, *dn_b;

		err = dnode_hold(base_os, base_obj, FTAG, &dn_a);
		if (err != 0)
			return (err);
		err = dnode_hold(side_os, side_obj, FTAG, &dn_b);
		if (err != 0) {
			dnode_rele(dn_a, FTAG);
			return (err);
		}

		same = B_TRUE;
		for (int i = 0; i < doi_a.doi_nblkptr; i++) {
			blkptr_t *bp_a = &dn_a->dn_phys->dn_blkptr[i];
			blkptr_t *bp_b = &dn_b->dn_phys->dn_blkptr[i];

			if (BP_IS_HOLE(bp_a) && BP_IS_HOLE(bp_b))
				continue;
			if (BP_IS_HOLE(bp_a) != BP_IS_HOLE(bp_b)) {
				same = B_FALSE;
				break;
			}
			if (BP_IS_EMBEDDED(bp_a) ||
			    BP_IS_EMBEDDED(bp_b)) {
				same = B_FALSE;
				break;
			}
			if (!ZIO_CHECKSUM_EQUAL(bp_a->blk_cksum,
			    bp_b->blk_cksum)) {
				same = B_FALSE;
				break;
			}
		}

		dnode_rele(dn_b, FTAG);
		dnode_rele(dn_a, FTAG);

		if (same) {
			*hystp = B_TRUE;
			return (0);
		}
	}

	/*
	 * Slow path: read and compare file data block by block.
	 * Empty files (max_offset == 0) already matched on size
	 * above, so they're hysterical.
	 */
	if (doi_a.doi_max_offset == 0) {
		*hystp = B_TRUE;
		return (0);
	}

	{
		uint64_t offset = 0;
		uint64_t size = doi_a.doi_max_offset;
		uint64_t blksz = doi_a.doi_data_block_size;
		void *buf_a, *buf_b;

		if (blksz == 0)
			blksz = SPA_OLD_MAXBLOCKSIZE;

		buf_a = kmem_alloc(blksz, KM_SLEEP);
		buf_b = kmem_alloc(blksz, KM_SLEEP);

		same = B_TRUE;
		while (offset < size) {
			uint64_t chunk = MIN(blksz, size - offset);

			err = dmu_read(base_os, base_obj, offset,
			    chunk, buf_a, DMU_READ_NO_PREFETCH);
			if (err != 0)
				break;

			err = dmu_read(side_os, side_obj, offset,
			    chunk, buf_b, DMU_READ_NO_PREFETCH);
			if (err != 0)
				break;

			if (memcmp(buf_a, buf_b, chunk) != 0) {
				same = B_FALSE;
				break;
			}

			offset += chunk;
		}

		kmem_free(buf_a, blksz);
		kmem_free(buf_b, blksz);

		if (err != 0)
			return (err);

		if (same)
			*hystp = B_TRUE;
	}

	return (0);
}

/*
 * Detect hysterical directory edits — a directory dnode was replaced
 * (different index, same path) but the directory's identity and
 * logical contents are unchanged.
 *
 * A directory is hysterical when:
 *   1. SA identity fields match (mode, uid, gid, flags, etc.).
 *   2. The ZAP has the same set of entry names.
 *   3. For each entry with the same name but different obj#: if
 *      the child is a file, it must itself be hysterical.  If the
 *      child is a directory, it's fine — the child directory owns
 *      its own edit status.
 *
 * Unlike files, we cannot compare raw ZAP data blocks — internal
 * hash ordering varies between independently allocated ZAP objects.
 */
static int
rebase_is_hysterical_dir(objset_t *base_os, uint64_t base_obj,
    objset_t *side_os, uint64_t side_obj, boolean_t *hystp)
{
	zap_attribute_t *za;
	zap_cursor_t zc;
	boolean_t same;
	int err;

	*hystp = B_FALSE;

	err = rebase_sa_identity_equal(base_os, base_obj,
	    side_os, side_obj, &same);
	if (err != 0)
		return (err);
	if (!same)
		return (0);

	/*
	 * SA identity matches.  Compare ZAP entries — every name
	 * in base must exist in side and vice versa.  File children
	 * with different obj# must themselves be hysterical.
	 */
	za = zap_attribute_alloc();

	for (zap_cursor_init(&zc, base_os, base_obj);
	    (err = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		uint64_t base_child = ZFS_DIRENT_OBJ(za->za_first_integer);
		uint64_t side_child;

		err = zap_lookup(side_os, side_obj, za->za_name,
		    8, 1, &side_child);
		if (err == ENOENT) {
			zap_cursor_fini(&zc);
			zap_attribute_free(za);
			return (0);
		}
		if (err != 0)
			goto fail;

		side_child = ZFS_DIRENT_OBJ(side_child);

		if (base_child != side_child) {
			dmu_object_info_t doi;

			err = dmu_object_info(side_os, side_child,
			    &doi);
			if (err != 0)
				goto fail;

			if (doi.doi_type !=
			    DMU_OT_DIRECTORY_CONTENTS) {
				boolean_t child_hyst;

				err = rebase_is_hysterical(base_os,
				    base_child, side_os, side_child,
				    &child_hyst);
				if (err != 0)
					goto fail;
				if (!child_hyst) {
					zap_cursor_fini(&zc);
					zap_attribute_free(za);
					return (0);
				}
			}
		}
	}

	if (err == ENOENT)
		err = 0;
	zap_cursor_fini(&zc);
	if (err != 0) {
		zap_attribute_free(za);
		return (err);
	}

	/* Every side entry must also exist in base. */
	for (zap_cursor_init(&zc, side_os, side_obj);
	    (err = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		uint64_t dummy;

		err = zap_lookup(base_os, base_obj, za->za_name,
		    8, 1, &dummy);
		if (err == ENOENT) {
			zap_cursor_fini(&zc);
			zap_attribute_free(za);
			return (0);
		}
		if (err != 0)
			goto fail;
	}

	if (err == ENOENT)
		err = 0;
	zap_cursor_fini(&zc);
	zap_attribute_free(za);

	if (err == 0)
		*hystp = B_TRUE;

	return (err);

fail:
	zap_cursor_fini(&zc);
	zap_attribute_free(za);
	return (err);
}

/*
 * Compare two directory ZAPs entry-by-entry, recording differences
 * in the changelist.
 *
 * Phase 1: iterate base entries.
 *   - Same name, same obj, directory  → always recurse (a directory's
 *     own metadata does not reflect changes to child dnodes).
 *   - Same name, same obj, file  → hysterical check (SA identity +
 *     data comparison); record EDIT if content differs.
 *   - Same name, diff obj  → check hysterical (files via SA +
 *     data, directories via SA + ZAP contents); record EDIT if
 *     real; recurse into directories.
 *   - Name only in base    → DELETE; recurse deleted directories.
 *
 * Phase 2: iterate side entries.
 *   - Name not in base     → ADD; recurse added directories.
 */
static int
rebase_diff_dir(objset_t *base_os, objset_t *side_os,
    uint64_t base_dir, uint64_t side_dir,
    const char *path, size_t pathlen,
    rebase_changelist_t *rcl, zap_attribute_t *za)
{
	zap_cursor_t zc;
	int err = 0;

	/* Phase 1: iterate base, find DELETEs and EDITs. */
	for (zap_cursor_init(&zc, base_os, base_dir);
	    (err = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		uint64_t base_child = ZFS_DIRENT_OBJ(za->za_first_integer);
		uint64_t side_child;
		dmu_object_info_t doi;
		char *cpath;
		size_t cpathlen;

		cpath = rebase_build_path(path, pathlen,
		    za->za_name, &cpathlen);

		err = zap_lookup(side_os, side_dir, za->za_name,
		    8, 1, &side_child);

		if (err == ENOENT) {
			/* Base only → DELETE. */
			err = dmu_object_info(base_os, base_child,
			    &doi);
			if (err != 0) {
				kmem_free(cpath, cpathlen);
				break;
			}

			rebase_record_change(rcl, RCT_DELETE,
			    base_child, cpath, cpathlen,
			    doi.doi_type);

			if (doi.doi_type ==
			    DMU_OT_DIRECTORY_CONTENTS) {
				err = rebase_walk_tree(base_os, rcl,
				    RCT_DELETE, base_child,
				    cpath, cpathlen, za);
			}

			kmem_free(cpath, cpathlen);
			if (err != 0)
				break;
			continue;
		}
		if (err != 0) {
			kmem_free(cpath, cpathlen);
			break;
		}

		side_child = ZFS_DIRENT_OBJ(side_child);

		if (base_child == side_child) {
			err = dmu_object_info(base_os,
			    base_child, &doi);
			if (err != 0) {
				kmem_free(cpath, cpathlen);
				break;
			}

			if (doi.doi_type ==
			    DMU_OT_DIRECTORY_CONTENTS) {
				/*
				 * Always recurse into subdirectories.
				 * A directory's bonus buffer does not
				 * reflect changes to child dnodes, so
				 * we cannot skip the walk.
				 */
				err = rebase_diff_dir(base_os,
				    side_os, base_child,
				    side_child, cpath,
				    cpathlen, rcl, za);
			} else {
				boolean_t hyst;

				err = rebase_is_hysterical(
				    base_os, base_child,
				    side_os, side_child,
				    &hyst);
				if (err != 0) {
					kmem_free(cpath,
					    cpathlen);
					break;
				}
				if (!hyst) {
					rebase_record_change(
					    rcl, RCT_EDIT,
					    side_child,
					    cpath, cpathlen,
					    doi.doi_type);
				}
			}

			kmem_free(cpath, cpathlen);
			if (err != 0)
				break;
			continue;
		}

		/*
		 * Different object at same path.
		 * Check for hysterical edits before recording.
		 */
		err = dmu_object_info(side_os, side_child, &doi);
		if (err != 0) {
			kmem_free(cpath, cpathlen);
			break;
		}

		if (doi.doi_type == DMU_OT_DIRECTORY_CONTENTS) {
			boolean_t hyst;

			err = rebase_is_hysterical_dir(base_os,
			    base_child, side_os, side_child,
			    &hyst);
			if (err != 0) {
				kmem_free(cpath, cpathlen);
				break;
			}
			if (!hyst) {
				rebase_record_change(rcl, RCT_EDIT,
				    side_child, cpath, cpathlen,
				    doi.doi_type);
			}
			err = rebase_diff_dir(base_os, side_os,
			    base_child, side_child,
			    cpath, cpathlen, rcl, za);
		} else {
			boolean_t hyst;

			err = rebase_is_hysterical(base_os,
			    base_child, side_os, side_child,
			    &hyst);
			if (err != 0) {
				kmem_free(cpath, cpathlen);
				break;
			}
			if (!hyst) {
				rebase_record_change(rcl, RCT_EDIT,
				    side_child, cpath, cpathlen,
				    doi.doi_type);
			}
		}

		kmem_free(cpath, cpathlen);
		if (err != 0)
			break;
	}

	if (err == ENOENT)
		err = 0;

	zap_cursor_fini(&zc);

	if (err != 0)
		return (err);

	/* Phase 2: iterate side, find ADDs. */
	for (zap_cursor_init(&zc, side_os, side_dir);
	    (err = zap_cursor_retrieve(&zc, za)) == 0;
	    zap_cursor_advance(&zc)) {
		uint64_t side_child = ZFS_DIRENT_OBJ(za->za_first_integer);
		uint64_t dummy;
		dmu_object_info_t doi;
		char *cpath;
		size_t cpathlen;

		err = zap_lookup(base_os, base_dir, za->za_name,
		    8, 1, &dummy);

		if (err == ENOENT) {
			/* Side only → ADD. */
			cpath = rebase_build_path(path, pathlen,
			    za->za_name, &cpathlen);

			err = dmu_object_info(side_os, side_child,
			    &doi);
			if (err != 0) {
				kmem_free(cpath, cpathlen);
				break;
			}

			rebase_record_change(rcl, RCT_ADD,
			    side_child, cpath, cpathlen,
			    doi.doi_type);

			if (doi.doi_type ==
			    DMU_OT_DIRECTORY_CONTENTS) {
				err = rebase_walk_tree(side_os, rcl,
				    RCT_ADD, side_child,
				    cpath, cpathlen, za);
			}

			kmem_free(cpath, cpathlen);
			if (err != 0)
				break;
			continue;
		}
		if (err != 0)
			break;
	}

	if (err == ENOENT)
		err = 0;

	zap_cursor_fini(&zc);
	return (err);
}

/*
 * Populate both changelists by diffing base against each side.
 */
static int
rebase_diff(rebase_state_t *rs)
{
	zap_attribute_t *za;
	int err;

	za = zap_attribute_alloc();

	err = rebase_diff_dir(rs->rs_base_os, rs->rs_left_os,
	    rs->rs_base_root, rs->rs_left_root,
	    "/", 2, &rs->rs_left_changes, za);

	if (err == 0) {
		err = rebase_diff_dir(rs->rs_base_os, rs->rs_right_os,
		    rs->rs_base_root, rs->rs_right_root,
		    "/", 2, &rs->rs_right_changes, za);
	}

	zap_attribute_free(za);
	return (err);
}

/*
 * Check whether the same dnode slot in two objsets holds the
 * same dnode, not a freed-and-reallocated slot.  Compares
 * ZPL_GEN — matching generation means the dnode was neither
 * freed nor reallocated.  Returns B_FALSE in *samep if the
 * object does not exist in either objset, has type NONE
 * (freed slot), or ZPL_GEN differs.
 */
static int
rebase_same_gen(objset_t *os_a, objset_t *os_b,
    uint64_t obj, boolean_t *samep)
{
	dmu_object_info_t doi;
	sa_handle_t *hdl_a = NULL, *hdl_b = NULL;
	uint64_t gen_a, gen_b;
	int err;

	*samep = B_FALSE;

	err = dmu_object_info(os_a, obj, &doi);
	if (err != 0 || doi.doi_type == DMU_OT_NONE)
		return (0);

	err = dmu_object_info(os_b, obj, &doi);
	if (err != 0 || doi.doi_type == DMU_OT_NONE)
		return (0);

	err = sa_handle_get(os_a, obj, NULL, SA_HDL_PRIVATE, &hdl_a);
	if (err != 0)
		return (err);

	err = sa_handle_get(os_b, obj, NULL, SA_HDL_PRIVATE, &hdl_b);
	if (err != 0) {
		sa_handle_destroy(hdl_a);
		return (err);
	}

	err = sa_lookup(hdl_a, ZPL_GEN, &gen_a, sizeof (gen_a));
	if (err != 0)
		goto out;

	err = sa_lookup(hdl_b, ZPL_GEN, &gen_b, sizeof (gen_b));
	if (err != 0)
		goto out;

	*samep = (gen_a == gen_b);

out:
	sa_handle_destroy(hdl_b);
	sa_handle_destroy(hdl_a);
	return (err);
}

/*
 * Collapse phase — second pass over a single changelist.
 *
 * Walk the rcl_by_obj index where entries with the same dnode
 * object number are adjacent (obj comparator, path tiebreaker).
 *
 * Pass 1 — move detection:
 *   An ADD+DELETE pair for the same obj# with matching ZPL_GEN
 *   is a rename/move.  Collapse to RCT_MOVE (content unchanged)
 *   or RCT_MOVE_EDIT (content differs from base).  The DELETE
 *   entry is removed from both AVL trees and freed; the ADD
 *   entry is promoted in-place with rc_old_path set to the
 *   DELETE's path.
 *
 * Pass 2 — hardlink detection:
 *   A remaining ADD whose obj# exists in base with matching
 *   ZPL_GEN is RCT_HARDLINK_ADD (new link to existing dnode).
 *   A remaining DELETE whose obj# still exists in the side
 *   with matching ZPL_GEN is RCT_HARDLINK_DELETE (link removed
 *   but dnode persists via other links).
 */
static int
rebase_collapse_changelist(rebase_changelist_t *rcl,
    objset_t *base_os, objset_t *side_os)
{
	rebase_change_t *rc;
	int err;

	/* Pass 1: move detection — collapse ADD+DELETE pairs. */
	rc = avl_first(&rcl->rcl_by_obj);
	while (rc != NULL) {
		uint64_t obj = rc->rc_obj;
		rebase_change_t *add = NULL, *del = NULL;
		rebase_change_t *p;

		/*
		 * Scan all entries in this obj# group, finding
		 * the first ADD and first DELETE.  After the loop,
		 * p points to the first entry of the next group.
		 */
		for (p = rc; p != NULL && p->rc_obj == obj;
		    p = AVL_NEXT(&rcl->rcl_by_obj, p)) {
			if (p->rc_type == RCT_ADD && add == NULL)
				add = p;
			else if (p->rc_type == RCT_DELETE &&
			    del == NULL)
				del = p;
		}

		if (add != NULL && del != NULL) {
			boolean_t same;

			err = rebase_same_gen(base_os, side_os,
			    obj, &same);
			if (err != 0)
				return (err);

			if (same) {
				boolean_t hyst;

				err = rebase_is_hysterical(base_os,
				    obj, side_os, obj, &hyst);
				if (err != 0)
					return (err);

				add->rc_type = hyst ?
				    RCT_MOVE : RCT_MOVE_EDIT;
				add->rc_old_path = del->rc_path;
				add->rc_old_pathlen =
				    del->rc_pathlen;

				avl_remove(&rcl->rcl_by_path, del);
				avl_remove(&rcl->rcl_by_obj, del);
				del->rc_path = NULL;
				kmem_free(del, sizeof (*del));
				rcl->rcl_count--;
			}
		}

		rc = p;
	}

	/* Pass 2: hardlink detection. */
	for (rc = avl_first(&rcl->rcl_by_obj); rc != NULL;
	    rc = AVL_NEXT(&rcl->rcl_by_obj, rc)) {
		boolean_t same;

		if (rc->rc_type == RCT_ADD) {
			err = rebase_same_gen(base_os, side_os,
			    rc->rc_obj, &same);
			if (err != 0)
				return (err);
			if (same)
				rc->rc_type = RCT_HARDLINK_ADD;
		} else if (rc->rc_type == RCT_DELETE) {
			err = rebase_same_gen(base_os, side_os,
			    rc->rc_obj, &same);
			if (err != 0)
				return (err);
			if (same)
				rc->rc_type = RCT_HARDLINK_DELETE;
		}
	}

	return (0);
}

/*
 * Run the collapse phase on both changelists.
 */
static int
rebase_collapse(rebase_state_t *rs)
{
	int err;

	err = rebase_collapse_changelist(&rs->rs_left_changes,
	    rs->rs_base_os, rs->rs_left_os);
	if (err != 0)
		return (err);

	return (rebase_collapse_changelist(&rs->rs_right_changes,
	    rs->rs_base_os, rs->rs_right_os));
}

/*
 * ================================================================
 * Cross-reference phase — conflict detection
 * ================================================================
 */

static void
rebase_manifest_init(rebase_manifest_t *rm)
{
	list_create(&rm->rm_conflicts, sizeof (rebase_conflict_t),
	    offsetof(rebase_conflict_t, rcf_node));
	rm->rm_nconflicts = 0;
}

static void
rebase_manifest_fini(rebase_manifest_t *rm)
{
	rebase_conflict_t *rcf;

	while ((rcf = list_remove_head(&rm->rm_conflicts)) != NULL) {
		kmem_free(rcf->rcf_path, rcf->rcf_pathlen);
		for (uint_t i = 0; i < rcf->rcf_nalt; i++)
			kmem_free(rcf->rcf_alt_paths[i],
			    strlen(rcf->rcf_alt_paths[i]) + 1);
		if (rcf->rcf_alt_paths != NULL)
			kmem_free(rcf->rcf_alt_paths,
			    rcf->rcf_nalt * sizeof (char *));
		kmem_free(rcf, sizeof (*rcf));
	}
	list_destroy(&rm->rm_conflicts);
}

/*
 * Find an existing conflict record for the given dnode object.
 * Used for hardlink deduplication — multiple paths to the same
 * dnode share a single conflict entry.
 */
static rebase_conflict_t *
rebase_manifest_find_obj(rebase_manifest_t *rm, uint64_t obj)
{
	rebase_conflict_t *rcf;

	for (rcf = list_head(&rm->rm_conflicts);
	    rcf != NULL; rcf = list_next(&rm->rm_conflicts, rcf)) {
		if (rcf->rcf_obj == obj)
			return (rcf);
	}
	return (NULL);
}

/*
 * Add a path to an existing conflict's alt_paths array.
 */
static void
rebase_conflict_add_alt(rebase_conflict_t *rcf,
    const char *path, size_t pathlen)
{
	uint_t n = rcf->rcf_nalt + 1;
	char **new_paths;

	new_paths = kmem_alloc(n * sizeof (char *), KM_SLEEP);
	if (rcf->rcf_alt_paths != NULL) {
		memcpy(new_paths, rcf->rcf_alt_paths,
		    rcf->rcf_nalt * sizeof (char *));
		kmem_free(rcf->rcf_alt_paths,
		    rcf->rcf_nalt * sizeof (char *));
	}

	new_paths[rcf->rcf_nalt] = kmem_alloc(pathlen, KM_SLEEP);
	memcpy(new_paths[rcf->rcf_nalt], path, pathlen);

	rcf->rcf_alt_paths = new_paths;
	rcf->rcf_nalt = n;
}

/*
 * Record a conflict in the manifest. Deduplicates by dnode
 * object number — if this obj already has a conflict, the
 * path is added as an alternate path instead.
 */
static void
rebase_record_conflict(rebase_manifest_t *rm,
    rebase_conflict_type_t type, uint64_t obj,
    const char *path, size_t pathlen)
{
	rebase_conflict_t *existing;
	rebase_conflict_t *rcf;

	existing = rebase_manifest_find_obj(rm, obj);
	if (existing != NULL) {
		rebase_conflict_add_alt(existing, path, pathlen);
		return;
	}

	rcf = kmem_zalloc(sizeof (*rcf), KM_SLEEP);
	rcf->rcf_type = type;
	rcf->rcf_obj = obj;
	rcf->rcf_pathlen = pathlen;
	rcf->rcf_path = kmem_alloc(pathlen, KM_SLEEP);
	memcpy(rcf->rcf_path, path, pathlen);

	list_insert_tail(&rm->rm_conflicts, rcf);
	rm->rm_nconflicts++;
}

/*
 * Classify a conflict for two entries at the same resolved path.
 */
static int
rebase_crossref_samepath(rebase_state_t *rs,
    rebase_change_t *lrc, rebase_change_t *rrc)
{
	boolean_t same;
	int err;

	/* Both EDIT — check for identical changes before flagging. */
	if ((lrc->rc_type == RCT_EDIT ||
	    lrc->rc_type == RCT_MOVE_EDIT) &&
	    (rrc->rc_type == RCT_EDIT ||
	    rrc->rc_type == RCT_MOVE_EDIT)) {
		err = rebase_is_hysterical(rs->rs_left_os, lrc->rc_obj,
		    rs->rs_right_os, rrc->rc_obj, &same);
		if (err != 0)
			return (err);
		if (same)
			return (0);

		rebase_record_conflict(&rs->rs_manifest,
		    RCONF_BOTH_MODIFIED, lrc->rc_obj,
		    lrc->rc_path, lrc->rc_pathlen);
		return (0);
	}

	/* Both ADD — check for identical content before flagging. */
	if ((lrc->rc_type == RCT_ADD ||
	    lrc->rc_type == RCT_HARDLINK_ADD) &&
	    (rrc->rc_type == RCT_ADD ||
	    rrc->rc_type == RCT_HARDLINK_ADD)) {
		err = rebase_is_hysterical(rs->rs_left_os, lrc->rc_obj,
		    rs->rs_right_os, rrc->rc_obj, &same);
		if (err != 0)
			return (err);
		if (same)
			return (0);

		rebase_record_conflict(&rs->rs_manifest,
		    RCONF_CREATE_CREATE, lrc->rc_obj,
		    lrc->rc_path, lrc->rc_pathlen);
		return (0);
	}

	/* Left EDIT, right DELETE — modify-delete. */
	if ((lrc->rc_type == RCT_EDIT ||
	    lrc->rc_type == RCT_MOVE_EDIT) &&
	    (rrc->rc_type == RCT_DELETE ||
	    rrc->rc_type == RCT_HARDLINK_DELETE)) {
		rebase_record_conflict(&rs->rs_manifest,
		    RCONF_MODIFY_DELETE, lrc->rc_obj,
		    lrc->rc_path, lrc->rc_pathlen);
		return (0);
	}

	/* Left DELETE, right EDIT — delete-modify. */
	if ((lrc->rc_type == RCT_DELETE ||
	    lrc->rc_type == RCT_HARDLINK_DELETE) &&
	    (rrc->rc_type == RCT_EDIT ||
	    rrc->rc_type == RCT_MOVE_EDIT)) {
		rebase_record_conflict(&rs->rs_manifest,
		    RCONF_DELETE_MODIFY, rrc->rc_obj,
		    rrc->rc_path, rrc->rc_pathlen);
		return (0);
	}

	/* Both DELETE — not a conflict. */
	if ((lrc->rc_type == RCT_DELETE ||
	    lrc->rc_type == RCT_HARDLINK_DELETE) &&
	    (rrc->rc_type == RCT_DELETE ||
	    rrc->rc_type == RCT_HARDLINK_DELETE)) {
		return (0);
	}

	/* Both sides moved something to this path. */
	if ((lrc->rc_type == RCT_MOVE ||
	    lrc->rc_type == RCT_MOVE_EDIT) &&
	    (rrc->rc_type == RCT_MOVE ||
	    rrc->rc_type == RCT_MOVE_EDIT)) {
		if (lrc->rc_obj != rrc->rc_obj) {
			rebase_record_conflict(&rs->rs_manifest,
			    RCONF_MOVE_DIVERGE, lrc->rc_obj,
			    lrc->rc_path, lrc->rc_pathlen);
			return (0);
		}

		/*
		 * Same dnode, same destination.  If both sides also
		 * edited content, check whether the edits are identical.
		 */
		if (lrc->rc_type == RCT_MOVE_EDIT &&
		    rrc->rc_type == RCT_MOVE_EDIT) {
			err = rebase_is_hysterical(rs->rs_left_os,
			    lrc->rc_obj, rs->rs_right_os,
			    rrc->rc_obj, &same);
			if (err != 0)
				return (err);
			if (!same) {
				rebase_record_conflict(&rs->rs_manifest,
				    RCONF_BOTH_MODIFIED, lrc->rc_obj,
				    lrc->rc_path, lrc->rc_pathlen);
			}
		}
		return (0);
	}

	/*
	 * MOVE destination collides with the other side's entry.
	 * One side moved a file TO this path; the other side has
	 * a change AT this path.
	 */
	if (lrc->rc_type == RCT_MOVE ||
	    lrc->rc_type == RCT_MOVE_EDIT ||
	    rrc->rc_type == RCT_MOVE ||
	    rrc->rc_type == RCT_MOVE_EDIT) {
		rebase_record_conflict(&rs->rs_manifest,
		    RCONF_MOVE_VS_EDIT, lrc->rc_obj,
		    lrc->rc_path, lrc->rc_pathlen);
		return (0);
	}

	return (0);
}

/*
 * Find the first entry in an rcl_by_obj tree with the given
 * object number, or NULL if none exists.
 */
static rebase_change_t *
rebase_find_by_obj(rebase_changelist_t *rcl, uint64_t obj)
{
	rebase_change_t search;
	avl_index_t where;
	rebase_change_t *rc;

	search.rc_obj = obj;
	search.rc_path = "";

	rc = avl_find(&rcl->rcl_by_obj, &search, &where);
	if (rc != NULL)
		return (rc);

	rc = avl_nearest(&rcl->rcl_by_obj, where, AVL_AFTER);
	if (rc != NULL && rc->rc_obj == obj)
		return (rc);

	return (NULL);
}

/*
 * Detect move divergence and move-vs-edit conflicts by
 * cross-referencing object numbers between the two changelists.
 */
static void
rebase_crossref_moves(rebase_state_t *rs)
{
	rebase_changelist_t *left = &rs->rs_left_changes;
	rebase_changelist_t *right = &rs->rs_right_changes;
	rebase_change_t *lrc, *rrc;

	for (lrc = avl_first(&left->rcl_by_obj); lrc != NULL;
	    lrc = AVL_NEXT(&left->rcl_by_obj, lrc)) {
		if (lrc->rc_type != RCT_MOVE &&
		    lrc->rc_type != RCT_MOVE_EDIT)
			continue;

		rrc = rebase_find_by_obj(right, lrc->rc_obj);
		if (rrc == NULL)
			continue;

		if (rrc->rc_type == RCT_MOVE ||
		    rrc->rc_type == RCT_MOVE_EDIT) {
			if (strcmp(lrc->rc_path, rrc->rc_path) != 0) {
				rebase_record_conflict(&rs->rs_manifest,
				    RCONF_MOVE_DIVERGE, lrc->rc_obj,
				    lrc->rc_path, lrc->rc_pathlen);
			}
		} else {
			rebase_record_conflict(&rs->rs_manifest,
			    RCONF_MOVE_VS_EDIT, lrc->rc_obj,
			    lrc->rc_path, lrc->rc_pathlen);
		}
	}

	/* Check right-side moves against left entries. */
	for (rrc = avl_first(&right->rcl_by_obj); rrc != NULL;
	    rrc = AVL_NEXT(&right->rcl_by_obj, rrc)) {
		if (rrc->rc_type != RCT_MOVE &&
		    rrc->rc_type != RCT_MOVE_EDIT)
			continue;

		lrc = rebase_find_by_obj(left, rrc->rc_obj);
		if (lrc == NULL)
			continue;

		/*
		 * Skip pairs where left is also a MOVE — already
		 * handled above (avoid double-reporting).
		 */
		if (lrc->rc_type == RCT_MOVE ||
		    lrc->rc_type == RCT_MOVE_EDIT)
			continue;

		rebase_record_conflict(&rs->rs_manifest,
		    RCONF_MOVE_VS_EDIT, rrc->rc_obj,
		    rrc->rc_path, rrc->rc_pathlen);
	}
}

/*
 * Detect directory-deletion-vs-edit conflicts. For each directory
 * DELETE on one side, find entries on the other side whose path
 * falls inside the deleted directory.  Only flag entries that are
 * exclusive to the other side (not already matched by the path
 * merge walk).
 */
static void
rebase_crossref_dir_deletes_one(rebase_manifest_t *rm,
    rebase_changelist_t *del_side, rebase_changelist_t *other_side)
{
	rebase_change_t *rc;

	for (rc = avl_first(&del_side->rcl_by_path); rc != NULL;
	    rc = AVL_NEXT(&del_side->rcl_by_path, rc)) {
		size_t dirlen;
		char *prefix;
		rebase_change_t search;
		avl_index_t where;
		rebase_change_t *orc;

		if (rc->rc_type != RCT_DELETE &&
		    rc->rc_type != RCT_HARDLINK_DELETE)
			continue;
		if (rc->rc_dn_type != DMU_OT_DIRECTORY_CONTENTS)
			continue;

		/*
		 * Build the prefix: dir_path + "/".
		 * Paths inside this directory sort after the
		 * prefix in strcmp order.
		 */
		dirlen = strlen(rc->rc_path);
		prefix = kmem_alloc(dirlen + 2, KM_SLEEP);
		memcpy(prefix, rc->rc_path, dirlen);
		prefix[dirlen] = '/';
		prefix[dirlen + 1] = '\0';

		/*
		 * Use AVL lookup to find the first entry in the
		 * other side's by_path tree >= prefix.
		 */
		search.rc_path = prefix;
		orc = avl_find(&other_side->rcl_by_path,
		    &search, &where);
		if (orc == NULL)
			orc = avl_nearest(&other_side->rcl_by_path,
			    where, AVL_AFTER);

		while (orc != NULL &&
		    strncmp(orc->rc_path, prefix, dirlen + 1) == 0) {
			rebase_change_t psearch;
			avl_index_t pwhere;

			/*
			 * Only flag if this path has no matching
			 * entry on the deleting side — otherwise
			 * the path merge already caught it.
			 */
			psearch.rc_path = orc->rc_path;
			if (avl_find(&del_side->rcl_by_path,
			    &psearch, &pwhere) == NULL) {
				rebase_record_conflict(rm,
				    RCONF_DIR_DELETE_VS_EDIT,
				    orc->rc_obj,
				    orc->rc_path, orc->rc_pathlen);
			}

			orc = AVL_NEXT(&other_side->rcl_by_path,
			    orc);
		}

		kmem_free(prefix, dirlen + 2);
	}
}

static void
rebase_crossref_dir_deletes(rebase_state_t *rs)
{
	rebase_crossref_dir_deletes_one(&rs->rs_manifest,
	    &rs->rs_left_changes, &rs->rs_right_changes);
	rebase_crossref_dir_deletes_one(&rs->rs_manifest,
	    &rs->rs_right_changes, &rs->rs_left_changes);
}

/*
 * Cross-reference left and right changelists to detect conflicts.
 * Three passes:
 *   1. Merge-walk both rcl_by_path trees to find same-path conflicts
 *   2. Walk by object number to find move divergence/move-vs-edit
 *   3. Prefix-match directory deletions against the other side
 */
static int
rebase_crossref(rebase_state_t *rs)
{
	rebase_changelist_t *left = &rs->rs_left_changes;
	rebase_changelist_t *right = &rs->rs_right_changes;
	rebase_change_t *lrc, *rrc;
	int err;

	/* Pass 1: path-based merge walk. */
	lrc = avl_first(&left->rcl_by_path);
	rrc = avl_first(&right->rcl_by_path);

	while (lrc != NULL && rrc != NULL) {
		int cmp = strcmp(lrc->rc_path, rrc->rc_path);

		if (cmp < 0) {
			lrc = AVL_NEXT(&left->rcl_by_path, lrc);
			continue;
		}
		if (cmp > 0) {
			rrc = AVL_NEXT(&right->rcl_by_path, rrc);
			continue;
		}

		err = rebase_crossref_samepath(rs, lrc, rrc);
		if (err != 0)
			return (err);

		lrc = AVL_NEXT(&left->rcl_by_path, lrc);
		rrc = AVL_NEXT(&right->rcl_by_path, rrc);
	}

	/* Pass 2: object-based move conflict detection. */
	rebase_crossref_moves(rs);

	/* Pass 3: directory deletion prefix matching. */
	rebase_crossref_dir_deletes(rs);

	return (0);
}

int
dsl_rebase(const char *left_ds, const char *right_ds, nvlist_t *outnvl)
{
	dsl_pool_t *dp;
	dsl_dataset_t *left, *right, *base;
	objset_t *left_os, *right_os, *base_os;
	rebase_state_t state;
	int err;

	(void) outnvl;

	/* Hold the pool from the left dataset name. */
	err = dsl_pool_hold(left_ds, FTAG, &dp);
	if (err != 0)
		return (err);

	/* Left must be a dataset (head), never a snapshot. */
	err = dsl_dataset_hold(dp, left_ds, FTAG, &left);
	if (err != 0) {
		dsl_pool_rele(dp, FTAG);
		return (err);
	}
	if (dsl_dataset_is_snapshot(left)) {
		dsl_dataset_rele(left, FTAG);
		dsl_pool_rele(dp, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Right may be a dataset (head) or a snapshot.
	 * dsl_dataset_hold handles both — '@' in the name
	 * selects the snapshot.
	 */
	err = dsl_dataset_hold(dp, right_ds, FTAG, &right);
	if (err != 0) {
		dsl_dataset_rele(left, FTAG);
		dsl_pool_rele(dp, FTAG);
		return (err);
	}

	/* Left and right must be different datasets. */
	if (dsl_dataset_phys(left)->ds_dir_obj ==
	    dsl_dataset_phys(right)->ds_dir_obj &&
	    !dsl_dataset_is_snapshot(right)) {
		err = SET_ERROR(EINVAL);
		goto rele_both;
	}

	/* Find the common ancestor snapshot. */
	err = rebase_find_common(dp, left, right, &base);
	if (err != 0)
		goto rele_both;

	/* Open objsets for all three. */
	err = dmu_objset_from_ds(left, &left_os);
	if (err != 0)
		goto rele_base;

	err = dmu_objset_from_ds(right, &right_os);
	if (err != 0)
		goto rele_base;

	err = dmu_objset_from_ds(base, &base_os);
	if (err != 0)
		goto rele_base;

	/* Set up SA attribute tables (idempotent if already mounted). */
	{
		sa_attr_type_t *sa_tbl;

		err = rebase_sa_setup(left_os, &sa_tbl);
		if (err != 0)
			goto rele_base;
		err = rebase_sa_setup(right_os, &sa_tbl);
		if (err != 0)
			goto rele_base;
		err = rebase_sa_setup(base_os, &sa_tbl);
		if (err != 0)
			goto rele_base;
	}

	/* Run all precondition checks. */
	err = rebase_check_preconditions(dp, left, right, base,
	    left_os, right_os, base_os);
	if (err != 0)
		goto rele_base;

	/* Populate rebase state. */
	state.rs_left = left;
	state.rs_right = right;
	state.rs_base = base;
	state.rs_left_os = left_os;
	state.rs_right_os = right_os;
	state.rs_base_os = base_os;

	/* Look up root directory object numbers. */
	err = rebase_master_lookup(left_os, ZFS_ROOT_OBJ,
	    &state.rs_left_root);
	if (err != 0)
		goto rele_base;

	err = rebase_master_lookup(right_os, ZFS_ROOT_OBJ,
	    &state.rs_right_root);
	if (err != 0)
		goto rele_base;

	err = rebase_master_lookup(base_os, ZFS_ROOT_OBJ,
	    &state.rs_base_root);
	if (err != 0)
		goto rele_base;

	/* Initialize changelists and manifest. */
	rebase_changelist_init(&state.rs_left_changes);
	rebase_changelist_init(&state.rs_right_changes);
	rebase_manifest_init(&state.rs_manifest);

	/* Diff phase: populate changelists. */
	err = rebase_diff(&state);

	/* Collapse phase: detect moves and hardlinks. */
	if (err == 0)
		err = rebase_collapse(&state);

	/* Cross-reference phase: detect conflicts. */
	if (err == 0)
		err = rebase_crossref(&state);

	if (err == 0) {
		/*
		 * Cross-reference complete.  Subsequent issues will
		 * fill in apply and emit phases here.
		 */
		err = SET_ERROR(ENOSYS);
	}

	rebase_manifest_fini(&state.rs_manifest);
	rebase_changelist_fini(&state.rs_left_changes);
	rebase_changelist_fini(&state.rs_right_changes);

rele_base:
	dsl_dataset_rele(base, FTAG);
rele_both:
	dsl_dataset_rele(right, FTAG);
	dsl_dataset_rele(left, FTAG);
	dsl_pool_rele(dp, FTAG);
	return (err);
}

int
dsl_rebase_finish(const char *dsname)
{
	(void) dsname;

	return (SET_ERROR(ENOSYS));
}

int
dsl_rebase_abort(const char *dsname)
{
	(void) dsname;

	return (SET_ERROR(ENOSYS));
}
