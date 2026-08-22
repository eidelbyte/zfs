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
 * ZFS rebase -- a git-rebase-like three-way merge for ZFS datasets.
 *
 * Given two datasets -- left and right -- auto-discover their common
 * ancestor snapshot (A), diff each side against it under the two-axis
 * change model (content ops and linkpool ops), merge membership per
 * path and content per linkpool, and produce a merged result
 * containing the left HEAD's state plus the right side's
 * non-conflicting changes, built in-chain on top of the left HEAD
 * using normal DMU write operations.
 *
 * The type contract and the model overview live in sys/dsl_rebase.h.
 */

#include <sys/zfs_context.h>
#include <sys/dsl_rebase.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_destroy.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_scan.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/nvpair.h>
#include <sys/sa.h>
#include <sys/zap.h>
#include <sys/zfs_acl.h>	/* zfs_sa.h needs zfs_acl_phys_t */
#include <sys/zfs_sa.h>
#include <sys/zfs_znode.h>

/*
 * Snapshot chain entry for common-ancestor discovery.
 * Member prefix rse_ = "rebase snap entry".
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
 * NOT included -- only its snapshots.
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
 * On success, *base is held with the caller's tag and the caller
 * must release it.
 */
static int
rebase_find_common(dsl_pool_t *dp, dsl_dataset_t *left,
    dsl_dataset_t *right, const void *tag, dsl_dataset_t **base)
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
	 * chain, the history is already linear -- nothing to rebase.
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

		err = dsl_dataset_hold_obj(dp, obj, tag, &snap);
		if (err != 0)
			break;

		search.rse_guid = dsl_dataset_phys(snap)->ds_guid;
		if (avl_find(&left_snaps, &search, NULL) != NULL) {
			*base = snap;
			rebase_snap_tree_destroy(&left_snaps);
			return (0);
		}

		obj = dsl_dataset_phys(snap)->ds_prev_snap_obj;
		dsl_dataset_rele(snap, tag);
	}

	rebase_snap_tree_destroy(&left_snaps);
	return (SET_ERROR(ENOENT));
}

/*
 * AVL comparators for rebase_change_t. Both indices use compound
 * keys (retrospective 2's fix, now the design): with path-scoped
 * content ops a path produces at most one record per side, so the
 * tiebreak component is cheap defense and deterministic ordering,
 * never a correctness crutch.
 */
static int
rebase_change_path_cmp(const void *a, const void *b)
{
	const rebase_change_t *la = a;
	const rebase_change_t *lb = b;
	int cmp;

	cmp = TREE_ISIGN(strcmp(la->rc_path, lb->rc_path));
	if (cmp != 0)
		return (cmp);
	return (TREE_CMP(la->rc_obj, lb->rc_obj));
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
	return (TREE_ISIGN(strcmp(la->rc_path, lb->rc_path)));
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
	rcl->rcl_count = 0;
}

/*
 * AVL comparators for the linkpool table: linkpools by shared
 * dnode object number, links by path (the reverse index).
 */
static int
rebase_linkpool_obj_cmp(const void *a, const void *b)
{
	const rebase_linkpool_t *la = a;
	const rebase_linkpool_t *lb = b;

	return (TREE_CMP(la->rlp_obj, lb->rlp_obj));
}

static int
rebase_linkpool_path_cmp(const void *a, const void *b)
{
	const rebase_linkpool_link_t *la = a;
	const rebase_linkpool_link_t *lb = b;

	return (TREE_ISIGN(strcmp(la->rlpl_path, lb->rlpl_path)));
}

/*
 * Linkpool table lifecycle. The by_obj tree owns the
 * rebase_linkpool_t nodes; each linkpool owns its links, which are
 * additionally indexed in the table-wide by_path reverse index.
 * The walk (zap-walk-basic) allocates linkpools and links; fini
 * handles both empty and populated tables.
 */
static void
rebase_linkpool_table_init(rebase_linkpool_table_t *rlpt)
{
	avl_create(&rlpt->rlpt_by_obj, rebase_linkpool_obj_cmp,
	    sizeof (rebase_linkpool_t),
	    offsetof(rebase_linkpool_t, rlp_avl));
	avl_create(&rlpt->rlpt_by_path, rebase_linkpool_path_cmp,
	    sizeof (rebase_linkpool_link_t),
	    offsetof(rebase_linkpool_link_t, rlpl_avl));
	rlpt->rlpt_count = 0;
}

static void
rebase_linkpool_table_fini(rebase_linkpool_table_t *rlpt)
{
	rebase_linkpool_t *rlp;
	void *cookie = NULL;

	while ((rlp = avl_destroy_nodes(&rlpt->rlpt_by_obj,
	    &cookie)) != NULL) {
		rebase_linkpool_link_t *rlpl;

		while ((rlpl = list_remove_head(&rlp->rlp_links)) != NULL) {
			avl_remove(&rlpt->rlpt_by_path, rlpl);
			kmem_free(rlpl->rlpl_path, rlpl->rlpl_pathlen);
			kmem_free(rlpl, sizeof (*rlpl));
		}
		list_destroy(&rlp->rlp_links);
		kmem_free(rlp, sizeof (*rlp));
	}
	avl_destroy(&rlpt->rlpt_by_obj);
	avl_destroy(&rlpt->rlpt_by_path);
	rlpt->rlpt_count = 0;
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
 * The ZPL properties that define name semantics. If any of these
 * differ between the three datasets, "same name" is not
 * well-defined and no phase of the diff may run.
 */
static const zfs_prop_t rebase_name_props[] = {
	ZFS_PROP_CASE,
	ZFS_PROP_NORMALIZE,
	ZFS_PROP_UTF8ONLY,
};

/*
 * Validate preconditions for a rebase operation.
 *
 * Checks (all three datasets are held, objsets opened):
 *   1. Common ancestor exists (already verified by find_common)
 *   2. All three objsets are ZPL filesystems -- zvol rebase is a
 *      possible v2 feature, and everything below reads MASTER_NODE
 *   3. No active scrub or resilver
 *   4. Encryption compatibility (both encrypted or both not)
 *   5. ZPL version >= 5 on all three (SA layout required)
 *   6. FUID table object identical across all three
 *   7. Name semantics (casesensitivity, normalization, utf8only)
 *      equal across all three, and restricted to casesensitivity =
 *      sensitive with normalization = none: the diff engine matches
 *      names byte-exactly, which is only correct when every name
 *      has exactly one stored form. Norm-aware matching is a
 *      possible v2 upgrade.
 *
 * Not checked (by design):
 *   - User holds: rebase only adds history after the left tip,
 *     never rewrites existing snapshots, so holds are harmless.
 *   - Right-side clones/snapshots: rebase only reads from the
 *     right side, never mutates it.  Clones of intermediate
 *     snapshots are unaffected.
 *   - Dedup: transparent at the DMU level.
 *   - SA layout: sa_setup() handles per-objset differences.
 */
static int
rebase_check_preconditions(dsl_pool_t *dp, dsl_dataset_t *left,
    dsl_dataset_t *right, objset_t *left_os, objset_t *right_os,
    objset_t *base_os)
{
	objset_t *oss[3];
	uint64_t vals[3];
	uint64_t v;
	int err;

	oss[0] = left_os;
	oss[1] = right_os;
	oss[2] = base_os;

	/* (2) All three must be ZPL filesystems. */
	for (int i = 0; i < 3; i++) {
		if (dmu_objset_type(oss[i]) != DMU_OST_ZFS)
			return (SET_ERROR(ENOTSUP));
	}

	/* (3) Scrub or resilver in progress -- avoid data races. */
	if (dsl_scan_active(dp->dp_scan))
		return (SET_ERROR(EBUSY));

	/* (4) Encryption -- both sides must agree. */
	if ((left->ds_dir->dd_crypto_obj == 0) !=
	    (right->ds_dir->dd_crypto_obj == 0))
		return (SET_ERROR(EACCES));

	/* (5) ZPL >= 5 (SA layout required) on all three. */
	for (int i = 0; i < 3; i++) {
		err = zfs_get_zplprop(oss[i], ZFS_PROP_VERSION, &v);
		if (err != 0)
			return (err);
		if (v < ZPL_VERSION_SA)
			return (SET_ERROR(ENOTSUP));
	}

	/* (6) FUID table object must match across all three. */
	for (int i = 0; i < 3; i++) {
		vals[i] = 0;
		(void) rebase_master_lookup(oss[i], ZFS_FUID_TABLES,
		    &vals[i]);
	}
	if (vals[0] != vals[1] || vals[0] != vals[2])
		return (SET_ERROR(ENOTSUP));

	/*
	 * (7) Name semantics must match across all three.
	 * zfs_get_zplprop reads the MASTER_NODE ZAP (or the objset
	 * cache) and supplies the creation default when a key was
	 * never written, so a filesystem created before one of these
	 * properties existed compares correctly against one carrying
	 * an explicit default.
	 */
	for (size_t p = 0; p < ARRAY_SIZE(rebase_name_props); p++) {
		for (int i = 0; i < 3; i++) {
			err = zfs_get_zplprop(oss[i],
			    rebase_name_props[p], &vals[i]);
			if (err != 0)
				return (err);
		}
		if (vals[0] != vals[1] || vals[0] != vals[2])
			return (SET_ERROR(ENOTSUP));

		/*
		 * Byte-exact name matching requires one stored form
		 * per name: case-insensitive or normalizing datasets
		 * can store the same logical name under different
		 * bytes (e.g. a rename that only changes case).
		 */
		if (rebase_name_props[p] == ZFS_PROP_CASE &&
		    vals[0] != ZFS_CASE_SENSITIVE)
			return (SET_ERROR(ENOTSUP));
		if (rebase_name_props[p] == ZFS_PROP_NORMALIZE &&
		    vals[0] != 0)
			return (SET_ERROR(ENOTSUP));
	}

	return (0);
}

/*
 * Hold everything one rebase pass needs: the pool, the left head,
 * the right head or snapshot, and the discovered common ancestor,
 * plus borrowed objset pointers for each (objsets obtained via
 * dmu_objset_from_ds carry no separate hold). The rebase_state_t
 * address is the hold tag, so rebase_rele() must receive the same
 * state. On error, nothing is left held.
 */
static int
rebase_hold(const char *left_ds, const char *right_ds, dsl_pool_t **dpp,
    rebase_state_t *rs)
{
	dsl_pool_t *dp;
	int err;

	err = dsl_pool_hold(left_ds, rs, &dp);
	if (err != 0)
		return (err);

	/* Left must be a dataset (head), never a snapshot. */
	err = dsl_dataset_hold(dp, left_ds, rs, &rs->rs_left);
	if (err != 0)
		goto rele_pool;
	if (dsl_dataset_is_snapshot(rs->rs_left)) {
		err = SET_ERROR(EINVAL);
		goto rele_left;
	}

	/*
	 * Right may be a dataset (head) or a snapshot.
	 * dsl_dataset_hold handles both -- '@' in the name
	 * selects the snapshot.
	 */
	err = dsl_dataset_hold(dp, right_ds, rs, &rs->rs_right);
	if (err != 0)
		goto rele_left;

	/* Left and right must be different datasets. */
	if (dsl_dataset_phys(rs->rs_left)->ds_dir_obj ==
	    dsl_dataset_phys(rs->rs_right)->ds_dir_obj &&
	    !dsl_dataset_is_snapshot(rs->rs_right)) {
		err = SET_ERROR(EINVAL);
		goto rele_right;
	}

	/* Find the common ancestor snapshot. */
	err = rebase_find_common(dp, rs->rs_left, rs->rs_right, rs,
	    &rs->rs_base);
	if (err != 0)
		goto rele_right;

	/* Borrowed objset pointers; no separate holds to manage. */
	err = dmu_objset_from_ds(rs->rs_left, &rs->rs_left_os);
	if (err == 0)
		err = dmu_objset_from_ds(rs->rs_right, &rs->rs_right_os);
	if (err == 0)
		err = dmu_objset_from_ds(rs->rs_base, &rs->rs_base_os);
	if (err != 0)
		goto rele_base;

	*dpp = dp;
	return (0);

rele_base:
	dsl_dataset_rele(rs->rs_base, rs);
rele_right:
	dsl_dataset_rele(rs->rs_right, rs);
rele_left:
	dsl_dataset_rele(rs->rs_left, rs);
rele_pool:
	dsl_pool_rele(dp, rs);
	return (err);
}

static void
rebase_rele(dsl_pool_t *dp, rebase_state_t *rs)
{
	dsl_dataset_rele(rs->rs_base, rs);
	dsl_dataset_rele(rs->rs_right, rs);
	dsl_dataset_rele(rs->rs_left, rs);
	dsl_pool_rele(dp, rs);
}

/*
 * Populate the rebase state after a successful hold: point every
 * diff read at an immutable source, capture the fork txg, look up
 * the three root directory objects, and initialize the changelists
 * and linkpool tables.
 */
static int
rebase_state_setup(rebase_state_t *rs, objset_t *left_snap_os,
    objset_t *right_snap_os)
{
	int err;

	/*
	 * The diff engine never walks a live objset: every left-side
	 * read goes through the fence-post snapshot. rs_left stays
	 * the head dataset -- it is the apply target, not a read
	 * source. When right came in as a live head it is fenced the
	 * same way through @%rebase-right-snap; a right given as a
	 * snapshot is already immutable (right_snap_os == NULL) and
	 * reads through the hold rebase_hold() took.
	 */
	rs->rs_left_os = left_snap_os;
	if (right_snap_os != NULL)
		rs->rs_right_os = right_snap_os;

	/*
	 * The fork point: the common ancestor's creation txg. A
	 * dnode whose newest birth txg is <= rs_fork_txg is
	 * untouched since the fork. Left and right allocate from
	 * the shared SPA counter concurrently after the fork, so
	 * txgs are only ever compared against rs_fork_txg, never
	 * across sides.
	 */
	rs->rs_fork_txg = dsl_dataset_phys(rs->rs_base)->ds_creation_txg;

	rs->rs_policy = REBASE_POLICY_NONE;

	/* Root directory object numbers, all from walk sources. */
	err = rebase_master_lookup(rs->rs_left_os, ZFS_ROOT_OBJ,
	    &rs->rs_left_root);
	if (err == 0)
		err = rebase_master_lookup(rs->rs_right_os, ZFS_ROOT_OBJ,
		    &rs->rs_right_root);
	if (err == 0)
		err = rebase_master_lookup(rs->rs_base_os, ZFS_ROOT_OBJ,
		    &rs->rs_base_root);
	if (err != 0)
		return (err);

	rebase_changelist_init(&rs->rs_left_changes);
	rebase_changelist_init(&rs->rs_right_changes);
	rebase_linkpool_table_init(&rs->rs_base_linkpools);
	rebase_linkpool_table_init(&rs->rs_left_linkpools);
	rebase_linkpool_table_init(&rs->rs_right_linkpools);

	return (0);
}

static void
rebase_state_teardown(rebase_state_t *rs)
{
	rebase_linkpool_table_fini(&rs->rs_right_linkpools);
	rebase_linkpool_table_fini(&rs->rs_left_linkpools);
	rebase_linkpool_table_fini(&rs->rs_base_linkpools);
	rebase_changelist_fini(&rs->rs_right_changes);
	rebase_changelist_fini(&rs->rs_left_changes);
}

/*
 * Destroy one fence-post snapshot on the way out. A failed destroy
 * would strand a %rebase snapshot and make the next rebase fail
 * with an unexplained EEXIST, so it is never silent: the failure is
 * logged, and when the operation itself had succeeded (the ENOSYS
 * not-implemented sentinel today, 0 once the engine completes) the
 * destroy error replaces that result. A real engine error is never
 * masked by cleanup trouble.
 */
static void
rebase_destroy_snap(const char *snapname, int *errp)
{
	int derr;

	derr = dsl_destroy_snapshot(snapname, B_FALSE);
	if (derr != 0 && derr != ENOENT) {
		zfs_dbgmsg("rebase: failed to destroy fence-post "
		    "snapshot %s: %d", snapname, derr);
		if (*errp == 0 || *errp == ENOSYS)
			*errp = derr;
	}
}

/*
 * Set up SA for an objset and return its attribute table. Nothing
 * has mounted these objsets (base and left are snapshots, right may
 * be an unmounted head), so the ZPL has not registered the
 * attribute table for us.
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
 * Per-walk context: the rebase state plus per-branch SA attribute
 * tables and one shared ZAP attribute buffer. Sharing one buffer
 * across recursion levels is safe: each level copies za_name into
 * its own child path before recursing, and ZAP cursors keep their
 * positions independently of the buffer.
 * Member prefix rwc_ = "rebase walk context".
 */
typedef struct rebase_walk_ctx {
	rebase_state_t	*rwc_rs;
	sa_attr_type_t	*rwc_left_sa;
	sa_attr_type_t	*rwc_base_sa;
	sa_attr_type_t	*rwc_right_sa;
	zap_attribute_t	*rwc_za;
} rebase_walk_ctx_t;

/*
 * Read ZPL_LINKS for one object. Every file on a ZPL >= 5 dataset
 * carries it (precondition 5); a missing attribute would silently
 * undermine linkpool discovery, so it is a hard error, never a
 * default.
 */
static int
rebase_get_nlink(objset_t *os, sa_attr_type_t *sa_tbl, uint64_t obj,
    uint64_t *nlinkp)
{
	sa_handle_t *hdl;
	int err;

	err = sa_handle_get(os, obj, NULL, SA_HDL_PRIVATE, &hdl);
	if (err != 0)
		return (err);

	err = sa_lookup(hdl, sa_tbl[ZPL_LINKS], nlinkp,
	    sizeof (*nlinkp));
	sa_handle_destroy(hdl);
	if (err == ENOENT)
		err = SET_ERROR(EIO);
	return (err);
}

/*
 * Record one visited path of a hardlinked dnode: upsert the
 * branch's linkpool (keyed by obj) and append this path as a link,
 * held in the owner's list and the table-wide by-path reverse
 * index. A linkpool is discovered at the FIRST link the walk
 * touches; nothing is ever searched.
 */
static void
rebase_linkpool_note(rebase_linkpool_table_t *rlpt, uint64_t obj,
    uint64_t nlink, const char *path, size_t pathlen)
{
	rebase_linkpool_t search, *rlp;
	rebase_linkpool_link_t *rlpl;
	avl_index_t where;

	search.rlp_obj = obj;
	rlp = avl_find(&rlpt->rlpt_by_obj, &search, &where);
	if (rlp == NULL) {
		rlp = kmem_zalloc(sizeof (*rlp), KM_SLEEP);
		rlp->rlp_obj = obj;
		rlp->rlp_nlink = nlink;
		rlp->rlp_state = REBASE_LINKPOOL_UNCLASSIFIED;
		list_create(&rlp->rlp_links,
		    sizeof (rebase_linkpool_link_t),
		    offsetof(rebase_linkpool_link_t, rlpl_node));
		avl_insert(&rlpt->rlpt_by_obj, rlp, where);
		rlpt->rlpt_count++;
	} else {
		/* Same dnode, same walk: ZPL_LINKS cannot change. */
		ASSERT3U(rlp->rlp_nlink, ==, nlink);
	}

	rlpl = kmem_zalloc(sizeof (*rlpl), KM_SLEEP);
	rlpl->rlpl_pathlen = pathlen;
	rlpl->rlpl_path = kmem_alloc(pathlen, KM_SLEEP);
	memcpy(rlpl->rlpl_path, path, pathlen);
	rlpl->rlpl_owner = rlp;
	list_insert_tail(&rlp->rlp_links, rlpl);
	avl_add(&rlpt->rlpt_by_path, rlpl);
	rlp->rlp_nfound++;
}

/*
 * Post-walk integrity check: every link of every linkpool must have
 * been seen. All links of a file live inside one dataset, and an
 * unlinked-open file's nlink is already decremented, so
 * walk-visible nlink always equals visible dir entries. A mismatch
 * means the linkpool is incomplete and merging with it would be
 * wrong: VERIFY-grade in debug builds, abort the rebase in
 * production.
 */
static int
rebase_linkpool_table_verify(rebase_linkpool_table_t *rlpt)
{
	for (rebase_linkpool_t *rlp = avl_first(&rlpt->rlpt_by_obj);
	    rlp != NULL; rlp = AVL_NEXT(&rlpt->rlpt_by_obj, rlp)) {
		if (rlp->rlp_nfound != rlp->rlp_nlink) {
			ASSERT3U(rlp->rlp_nfound, ==, rlp->rlp_nlink);
			return (SET_ERROR(EIO));
		}
	}
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
 * Per-path three-slot diff analysis: the left, base, and right
 * objects visible at one path (0 = absent on that side). This is
 * where hysterical-detect and standalone-diff land; until then
 * every visit is a no-op and the overall operation still exits
 * with ENOSYS.
 */
static int
rebase_walk_diff(rebase_walk_ctx_t *rwc, const char *path,
    size_t pathlen, uint64_t left_obj, uint64_t base_obj,
    uint64_t right_obj)
{
	(void) rwc;
	(void) path;
	(void) pathlen;
	(void) left_obj;
	(void) base_obj;
	(void) right_obj;

	return (0);
}

static int rebase_walk_dir(rebase_walk_ctx_t *rwc, uint64_t left_dir,
    uint64_t base_dir, uint64_t right_dir, const char *path,
    size_t pathlen);

/*
 * Visit one name with its up-to-three objects. Linkpool accounting
 * runs per branch for every non-directory slot (directories are
 * never linkpool members: ZPL forbids hardlinked dirs, and a dir's
 * ZPL_LINKS counts subdir back-references). The slot triple then
 * goes to walk_diff, and any directory slots are recursed --
 * including unchanged ones, because a child edit rewrites the
 * child dnode without touching the parent ZAP.
 */
static int
rebase_walk_visit(rebase_walk_ctx_t *rwc, const char *parent,
    size_t parentlen, const char *name, uint64_t left_obj,
    uint64_t base_obj, uint64_t right_obj)
{
	rebase_state_t *rs = rwc->rwc_rs;
	objset_t *oss[3];
	sa_attr_type_t *tbls[3];
	rebase_linkpool_table_t *rlpts[3];
	uint64_t objs[3];
	boolean_t isdir[3];
	char *cpath;
	size_t cpathlen;
	int err = 0;

	oss[0] = rs->rs_left_os;
	oss[1] = rs->rs_base_os;
	oss[2] = rs->rs_right_os;
	tbls[0] = rwc->rwc_left_sa;
	tbls[1] = rwc->rwc_base_sa;
	tbls[2] = rwc->rwc_right_sa;
	rlpts[0] = &rs->rs_left_linkpools;
	rlpts[1] = &rs->rs_base_linkpools;
	rlpts[2] = &rs->rs_right_linkpools;
	objs[0] = left_obj;
	objs[1] = base_obj;
	objs[2] = right_obj;

	cpath = rebase_build_path(parent, parentlen, name, &cpathlen);

	for (int i = 0; i < 3; i++) {
		dmu_object_info_t doi;
		uint64_t nlink;

		isdir[i] = B_FALSE;
		if (objs[i] == 0)
			continue;

		err = dmu_object_info(oss[i], objs[i], &doi);
		if (err != 0)
			goto out;

		if (doi.doi_type == DMU_OT_DIRECTORY_CONTENTS) {
			isdir[i] = B_TRUE;
			continue;
		}

		err = rebase_get_nlink(oss[i], tbls[i], objs[i],
		    &nlink);
		if (err != 0)
			goto out;
		if (nlink > 1)
			rebase_linkpool_note(rlpts[i], objs[i], nlink,
			    cpath, cpathlen);
	}

	err = rebase_walk_diff(rwc, cpath, cpathlen, left_obj,
	    base_obj, right_obj);
	if (err != 0)
		goto out;

	/*
	 * Recurse into whichever slots are directories. An absent
	 * or non-directory slot contributes nothing below this path.
	 */
	if (isdir[0] || isdir[1] || isdir[2]) {
		err = rebase_walk_dir(rwc,
		    isdir[0] ? objs[0] : 0,
		    isdir[1] ? objs[1] : 0,
		    isdir[2] ? objs[2] : 0,
		    cpath, cpathlen);
	}

out:
	kmem_free(cpath, cpathlen);
	return (err);
}

/*
 * Walk one directory level three ways: iterate the union of names,
 * visiting base's names first (with left and right matched by
 * lookup), then left's names absent from base, then right's names
 * absent from both. A dir argument of 0 means that side has no
 * directory at this path.
 *
 * Delete-queue orphans (the ZFS_UNLINKED_SET) are pathless and
 * carry nlink == 0: a path-driven walk never encounters them and
 * must not go looking. They cannot skew the linkpool VERIFY,
 * because queue residency implies the last dir entry is already
 * gone.
 */
static int
rebase_walk_dir(rebase_walk_ctx_t *rwc, uint64_t left_dir,
    uint64_t base_dir, uint64_t right_dir, const char *path,
    size_t pathlen)
{
	rebase_state_t *rs = rwc->rwc_rs;
	zap_attribute_t *za = rwc->rwc_za;
	zap_cursor_t zc;
	int err = 0;

	/* Phase 1: every name in base, with left and right matched. */
	if (base_dir != 0) {
		for (zap_cursor_init(&zc, rs->rs_base_os, base_dir);
		    (err = zap_cursor_retrieve(&zc, za)) == 0;
		    zap_cursor_advance(&zc)) {
			uint64_t b_obj, l_obj, r_obj, v;

			b_obj = ZFS_DIRENT_OBJ(za->za_first_integer);

			l_obj = 0;
			if (left_dir != 0) {
				err = zap_lookup(rs->rs_left_os,
				    left_dir, za->za_name, 8, 1, &v);
				if (err == 0)
					l_obj = ZFS_DIRENT_OBJ(v);
				else if (err != ENOENT)
					break;
			}

			r_obj = 0;
			if (right_dir != 0) {
				err = zap_lookup(rs->rs_right_os,
				    right_dir, za->za_name, 8, 1, &v);
				if (err == 0)
					r_obj = ZFS_DIRENT_OBJ(v);
				else if (err != ENOENT)
					break;
			}

			err = rebase_walk_visit(rwc, path, pathlen,
			    za->za_name, l_obj, b_obj, r_obj);
			if (err != 0)
				break;
		}
		zap_cursor_fini(&zc);
		if (err == ENOENT)
			err = 0;
		if (err != 0)
			return (err);
	}

	/* Phase 2: names only in left. */
	if (left_dir != 0) {
		for (zap_cursor_init(&zc, rs->rs_left_os, left_dir);
		    (err = zap_cursor_retrieve(&zc, za)) == 0;
		    zap_cursor_advance(&zc)) {
			uint64_t l_obj, r_obj, v;

			l_obj = ZFS_DIRENT_OBJ(za->za_first_integer);

			if (base_dir != 0) {
				err = zap_lookup(rs->rs_base_os,
				    base_dir, za->za_name, 8, 1, &v);
				if (err == 0)
					continue; /* phase 1 visited */
				if (err != ENOENT)
					break;
			}

			r_obj = 0;
			if (right_dir != 0) {
				err = zap_lookup(rs->rs_right_os,
				    right_dir, za->za_name, 8, 1, &v);
				if (err == 0)
					r_obj = ZFS_DIRENT_OBJ(v);
				else if (err != ENOENT)
					break;
			}

			err = rebase_walk_visit(rwc, path, pathlen,
			    za->za_name, l_obj, 0, r_obj);
			if (err != 0)
				break;
		}
		zap_cursor_fini(&zc);
		if (err == ENOENT)
			err = 0;
		if (err != 0)
			return (err);
	}

	/* Phase 3: names only in right. */
	if (right_dir != 0) {
		for (zap_cursor_init(&zc, rs->rs_right_os, right_dir);
		    (err = zap_cursor_retrieve(&zc, za)) == 0;
		    zap_cursor_advance(&zc)) {
			uint64_t r_obj, v;

			r_obj = ZFS_DIRENT_OBJ(za->za_first_integer);

			if (base_dir != 0) {
				err = zap_lookup(rs->rs_base_os,
				    base_dir, za->za_name, 8, 1, &v);
				if (err == 0)
					continue; /* phase 1 visited */
				if (err != ENOENT)
					break;
			}
			if (left_dir != 0) {
				err = zap_lookup(rs->rs_left_os,
				    left_dir, za->za_name, 8, 1, &v);
				if (err == 0)
					continue; /* phase 2 visited */
				if (err != ENOENT)
					break;
			}

			err = rebase_walk_visit(rwc, path, pathlen,
			    za->za_name, 0, 0, r_obj);
			if (err != 0)
				break;
		}
		zap_cursor_fini(&zc);
		if (err == ENOENT)
			err = 0;
		if (err != 0)
			return (err);
	}

	return (0);
}

/*
 * The walk phase: set up SA on the three read sources, walk the
 * union of the trees from the roots, and verify linkpool
 * completeness on all three tables.
 */
static int
rebase_walk(rebase_state_t *rs)
{
	rebase_walk_ctx_t rwc;
	int err;

	rwc.rwc_rs = rs;
	err = rebase_sa_setup(rs->rs_left_os, &rwc.rwc_left_sa);
	if (err == 0)
		err = rebase_sa_setup(rs->rs_base_os, &rwc.rwc_base_sa);
	if (err == 0)
		err = rebase_sa_setup(rs->rs_right_os,
		    &rwc.rwc_right_sa);
	if (err != 0)
		return (err);

	rwc.rwc_za = zap_attribute_alloc();

	err = rebase_walk_dir(&rwc, rs->rs_left_root,
	    rs->rs_base_root, rs->rs_right_root, "/", 2);

	zap_attribute_free(rwc.rwc_za);

	if (err == 0)
		err = rebase_linkpool_table_verify(
		    &rs->rs_base_linkpools);
	if (err == 0)
		err = rebase_linkpool_table_verify(
		    &rs->rs_left_linkpools);
	if (err == 0)
		err = rebase_linkpool_table_verify(
		    &rs->rs_right_linkpools);

	return (err);
}

int
dsl_rebase(const char *left_ds, const char *right_ds, nvlist_t *outnvl)
{
	dsl_pool_t *dp;
	dsl_dataset_t *left_snap, *right_snap;
	objset_t *left_snap_os, *right_snap_os;
	rebase_state_t state;
	char *snapname, *right_snapname;
	boolean_t right_is_head;
	int err;

	(void) outnvl;

	memset(&state, 0, sizeof (state));
	right_snap = NULL;
	right_snap_os = NULL;
	right_snapname = NULL;

	/*
	 * Pass 1 -- validate. The fence-post snapshots may only be
	 * created once preconditions pass, but snapshot creation is
	 * a sync task and would deadlock against this thread's own
	 * pool hold, so validation is a complete hold/rele cycle of
	 * its own.
	 */
	err = rebase_hold(left_ds, right_ds, &dp, &state);
	if (err != 0)
		return (err);
	err = rebase_check_preconditions(dp, state.rs_left,
	    state.rs_right, state.rs_left_os, state.rs_right_os,
	    state.rs_base_os);
	right_is_head = !dsl_dataset_is_snapshot(state.rs_right);
	rebase_rele(dp, &state);
	if (err != 0)
		return (err);

	/*
	 * Fence-post snapshots. Creation commits the ZIL, so walked
	 * on-disk state equals logical state; the diff engine never
	 * walks a live objset. The left snapshot is the read source
	 * for every left-side diff access and, once the apply phase
	 * exists, the universal rollback target. A right side given
	 * as a live head gets the same fence; a right given as a
	 * snapshot is already immutable. EEXIST here means a
	 * previous rebase left its snapshot behind; recovery is the
	 * abort path's business.
	 */
	snapname = kmem_asprintf("%s@%s", left_ds,
	    ZFS_REBASE_SNAP_SUFFIX);
	err = dmu_objset_snapshot_one(left_ds, ZFS_REBASE_SNAP_SUFFIX);
	if (err != 0) {
		kmem_strfree(snapname);
		return (err);
	}
	if (right_is_head) {
		right_snapname = kmem_asprintf("%s@%s", right_ds,
		    ZFS_REBASE_RIGHT_SNAP_SUFFIX);
		err = dmu_objset_snapshot_one(right_ds,
		    ZFS_REBASE_RIGHT_SNAP_SUFFIX);
		if (err != 0)
			goto destroy_snaps;
	}

	/*
	 * Pass 2 -- re-hold and re-validate (the world may have
	 * changed between the passes; the checks are cheap), then
	 * build the rebase state. Everything the diff reads from
	 * here on is an immutable snapshot.
	 */
	err = rebase_hold(left_ds, right_ds, &dp, &state);
	if (err != 0)
		goto destroy_snaps;

	err = rebase_check_preconditions(dp, state.rs_left,
	    state.rs_right, state.rs_left_os, state.rs_right_os,
	    state.rs_base_os);
	if (err != 0)
		goto rele;

	err = dsl_dataset_hold(dp, snapname, FTAG, &left_snap);
	if (err != 0)
		goto rele;
	err = dmu_objset_from_ds(left_snap, &left_snap_os);
	if (err != 0)
		goto rele_snap;

	if (right_is_head) {
		err = dsl_dataset_hold(dp, right_snapname, FTAG,
		    &right_snap);
		if (err != 0)
			goto rele_snap;
		err = dmu_objset_from_ds(right_snap, &right_snap_os);
		if (err != 0)
			goto rele_right_snap;
	}

	err = rebase_state_setup(&state, left_snap_os, right_snap_os);
	if (err != 0)
		goto rele_right_snap;

	/* Walk the trees and build the linkpool tables. */
	err = rebase_walk(&state);

	/*
	 * Walk complete.  Subsequent issues fill in the diff
	 * classification, cross-reference, emit, and apply phases
	 * here; until they land, a successful walk still exits
	 * with ENOSYS.
	 */
	if (err == 0)
		err = SET_ERROR(ENOSYS);

	rebase_state_teardown(&state);
rele_right_snap:
	if (right_snap != NULL)
		dsl_dataset_rele(right_snap, FTAG);
rele_snap:
	dsl_dataset_rele(left_snap, FTAG);
rele:
	rebase_rele(dp, &state);
destroy_snaps:
	/*
	 * While the engine is diff-only, a rebase leaves nothing
	 * behind: both fence-posts are destroyed on every exit. Once
	 * the apply phase lands, the success path keeps them until
	 * finish/abort. (Destruction is a sync task: no holds may be
	 * outstanding.)
	 */
	if (right_snapname != NULL) {
		rebase_destroy_snap(right_snapname, &err);
		kmem_strfree(right_snapname);
	}
	rebase_destroy_snap(snapname, &err);
	kmem_strfree(snapname);
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
