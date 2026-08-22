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
#include <sys/dnode.h>
#include <sys/dbuf.h>
#include <sys/nvpair.h>
#include <sys/sa.h>
#include <sys/spa.h>
#include <sys/zap.h>
#include <sys/zfs_acl.h>	/* zfs_sa.h needs zfs_acl_phys_t */
#include <sys/zfs_sa.h>
#include <sys/zfs_znode.h>
#include <sys/zio_checksum.h>

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
 * The pool-global $ORIGIN snapshot is not an ancestor. On any pool
 * at or past SPA_VERSION_ORIGIN, every plain-created dataset is
 * silently cloned from dp_origin_snap (dsl_dataset_create_sync_dd),
 * so every ds_prev_snap_obj chain in the pool eventually reaches
 * $ORIGIN@$ORIGIN -- two genuinely unrelated datasets would always
 * "share" it. It carries no filesystem relationship (and its objset
 * may not even be opened: dmu_objset_open_impl VERIFYs against it),
 * so both chain walks treat it as the end of real ancestry.
 */
static uint64_t
rebase_origin_snap_obj(dsl_pool_t *dp)
{
	if (dp->dp_origin_snap == NULL)
		return (0);
	return (dp->dp_origin_snap->ds_object);
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
	uint64_t origin_obj = rebase_origin_snap_obj(dp);
	uint64_t obj;
	int err = 0;

	avl_create(tree, rebase_snap_entry_cmp,
	    sizeof (rebase_snap_entry_t),
	    offsetof(rebase_snap_entry_t, rse_node));

	obj = dsl_dataset_phys(ds)->ds_prev_snap_obj;

	while (obj != 0 && obj != origin_obj) {
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
	uint64_t origin_obj = rebase_origin_snap_obj(dp);
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

	while (obj != 0 && obj != origin_obj) {
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

	/*
	 * (3) Scrub or resilver in progress. The predicate is the
	 * scan state machine being engaged -- dsl_scan_scrubbing()
	 * counts a PAUSED scrub too, which is still a scrub in
	 * progress -- and deliberately NOT dsl_scan_active(), which
	 * would miss paused scrubs while spuriously tripping on
	 * background cleanup a rebase does not conflict with (async
	 * destroys, pending frees, livelist deletes).
	 */
	if (dsl_scan_scrubbing(dp) || dsl_scan_resilvering(dp))
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
 * Transition from the short-hold phase to the long-running phase,
 * per the dsl_pool.c configuration-lock formula (the pattern
 * dmu_diff uses): place long holds on every dataset that stays
 * held, then drop the pool config lock, so a running rebase stops
 * blocking pool-wide DSL state changes (dataset creation and
 * destruction, renames, property setting) for its whole duration.
 * The long holds also make a concurrent destroy of any side fail
 * with EBUSY instead of racing the walk -- exactly the behavior a
 * running rebase wants. left_snap and right_snap are the
 * fence-post holds (right_snap NULL when right came in as a
 * snapshot). All long holds are tagged with the state address,
 * like the plain holds they shadow.
 */
static void
rebase_long_hold(dsl_pool_t *dp, rebase_state_t *rs,
    dsl_dataset_t *left_snap, dsl_dataset_t *right_snap)
{
	dsl_dataset_long_hold(rs->rs_left, rs);
	dsl_dataset_long_hold(rs->rs_right, rs);
	dsl_dataset_long_hold(rs->rs_base, rs);
	dsl_dataset_long_hold(left_snap, rs);
	if (right_snap != NULL)
		dsl_dataset_long_hold(right_snap, rs);
	dsl_pool_rele(dp, rs);
}

/*
 * Drop the long holds placed by rebase_long_hold(). The plain
 * holds remain and are released by the caller afterward -- legal
 * without the pool lock, per the same formula. The fence-post
 * snapshots must be fully released before the destroy-on-exit
 * sync task runs, or it would see its own EBUSY.
 */
static void
rebase_long_rele(rebase_state_t *rs, dsl_dataset_t *left_snap,
    dsl_dataset_t *right_snap)
{
	if (right_snap != NULL)
		dsl_dataset_long_rele(right_snap, rs);
	dsl_dataset_long_rele(left_snap, rs);
	dsl_dataset_long_rele(rs->rs_base, rs);
	dsl_dataset_long_rele(rs->rs_right, rs);
	dsl_dataset_long_rele(rs->rs_left, rs);
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
 * Walk slot indices: the fixed order of the three read sources in
 * every per-slot array (matching the (left, base, right) argument
 * order used throughout the walk).
 */
#define	REBASE_WALK_LEFT	0
#define	REBASE_WALK_BASE	1
#define	REBASE_WALK_RIGHT	2
#define	REBASE_WALK_NSLOTS	3

/*
 * Per-walk context: the rebase state, per-slot objsets and SA
 * attribute tables, one shared ZAP attribute buffer, and running
 * classification counters. Sharing one ZAP buffer across recursion
 * levels is safe: each level copies za_name into its own child path
 * before recursing, and ZAP cursors keep their positions
 * independently of the buffer.
 * Member prefix rwc_ = "rebase walk context".
 */
typedef struct rebase_walk_ctx {
	rebase_state_t	*rwc_rs;
	objset_t	*rwc_os[REBASE_WALK_NSLOTS];
	sa_attr_type_t	*rwc_sa[REBASE_WALK_NSLOTS];
	zap_attribute_t	*rwc_za;
	uint64_t	rwc_nvisited;
	uint64_t	rwc_nhysterical_left;
	uint64_t	rwc_nhysterical_right;
	uint64_t	rwc_nlinked;
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
 * means on-disk state is corrupt (a wrong ZPL_LINKS) and merging
 * with the incomplete linkpool would be wrong. Corrupt input is an
 * error, never an assertion -- asserts are for states the code
 * cannot reach, and debug builds must fail this ioctl exactly like
 * production does.
 */
static int
rebase_linkpool_table_verify(rebase_linkpool_table_t *rlpt)
{
	for (rebase_linkpool_t *rlp = avl_first(&rlpt->rlpt_by_obj);
	    rlp != NULL; rlp = AVL_NEXT(&rlpt->rlpt_by_obj, rlp)) {
		if (rlp->rlp_nfound != rlp->rlp_nlink) {
			zfs_dbgmsg("rebase: linkpool obj %llu "
			    "incomplete: found %llu links, "
			    "ZPL_LINKS says %llu",
			    (u_longlong_t)rlp->rlp_obj,
			    (u_longlong_t)rlp->rlp_nfound,
			    (u_longlong_t)rlp->rlp_nlink);
			return (SET_ERROR(EIO));
		}
	}
	return (0);
}

/*
 * Linkpool membership lookup by path: an O(log h) find on the
 * table's by-path reverse index, never a scan. Returns the owning
 * linkpool, or NULL when the path is not a linkpool member on this
 * branch.
 */
static rebase_linkpool_t *
rebase_linkpool_of(rebase_linkpool_table_t *rlpt, const char *path)
{
	rebase_linkpool_link_t search, *rlpl;

	/* Search key only; the (uintptr_t) hop satisfies -Wcast-qual. */
	search.rlpl_path = (char *)(uintptr_t)path;
	rlpl = avl_find(&rlpt->rlpt_by_path, &search, NULL);
	return (rlpl == NULL ? NULL : rlpl->rlpl_owner);
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
 * Compare one fixed-size (uint64_t) SA attribute across two
 * handles. The handles may belong to different objsets, so the
 * attribute id is mapped through each objset's own SA table by the
 * caller and passed per handle. Both-absent counts as equal;
 * present-vs-absent counts as different.
 */
static int
rebase_sa_cmp_uint64(sa_handle_t *hdl_a, sa_attr_type_t attr_a,
    sa_handle_t *hdl_b, sa_attr_type_t attr_b, boolean_t *samep)
{
	uint64_t va = 0, vb = 0;
	int ea, eb;

	*samep = B_FALSE;

	ea = sa_lookup(hdl_a, attr_a, &va, sizeof (va));
	eb = sa_lookup(hdl_b, attr_b, &vb, sizeof (vb));

	if (ea == ENOENT && eb == ENOENT) {
		*samep = B_TRUE;
		return (0);
	}
	if (ea != 0 && ea != ENOENT)
		return (ea);
	if (eb != 0 && eb != ENOENT)
		return (eb);
	if (ea != eb)
		return (0);

	*samep = (va == vb);
	return (0);
}

/*
 * Compare one variable-length SA attribute across two handles,
 * with the same per-handle attribute mapping and absence rules as
 * rebase_sa_cmp_uint64.
 */
static int
rebase_sa_cmp_var(sa_handle_t *hdl_a, sa_attr_type_t attr_a,
    sa_handle_t *hdl_b, sa_attr_type_t attr_b, boolean_t *samep)
{
	void *buf_a, *buf_b;
	int sz_a, sz_b;
	int ea, eb, err;

	*samep = B_FALSE;

	ea = sa_size(hdl_a, attr_a, &sz_a);
	eb = sa_size(hdl_b, attr_b, &sz_b);

	if (ea == ENOENT && eb == ENOENT) {
		*samep = B_TRUE;
		return (0);
	}
	if (ea != 0 && ea != ENOENT)
		return (ea);
	if (eb != 0 && eb != ENOENT)
		return (eb);
	if (ea != eb || sz_a != sz_b)
		return (0);
	if (sz_a == 0) {
		*samep = B_TRUE;
		return (0);
	}

	buf_a = kmem_alloc(sz_a, KM_SLEEP);
	buf_b = kmem_alloc(sz_b, KM_SLEEP);

	err = sa_lookup(hdl_a, attr_a, buf_a, sz_a);
	if (err == 0)
		err = sa_lookup(hdl_b, attr_b, buf_b, sz_b);
	if (err == 0)
		*samep = (memcmp(buf_a, buf_b, sz_a) == 0);

	kmem_free(buf_a, sz_a);
	kmem_free(buf_b, sz_b);
	return (err);
}

/*
 * SA identity attributes: the fields that constitute what a file
 * IS, as opposed to bookkeeping about it. Excluded on purpose:
 * timestamps and ZPL_GEN (rename-on-save always refreshes them, so
 * including them would make every hysterical edit look real),
 * ZPL_LINKS and ZPL_PARENT (linkpool-axis bookkeeping, never
 * content), ZPL_XATTR and ZPL_DXATTR (physical representation;
 * xattrs are compared logically by rebase_xattr_equal), and
 * ZPL_SCANSTAMP (a scanner cache, not identity).
 */
static const int rebase_identity_fixed[] = {
	ZPL_MODE, ZPL_UID, ZPL_GID, ZPL_FLAGS,
	ZPL_RDEV, ZPL_PROJID, ZPL_SIZE, ZPL_DACL_COUNT
};

static const int rebase_identity_var[] = {
	ZPL_DACL_ACES, ZPL_SYMLINK
};

/*
 * Compare the SA identity of two objects. For directories ZPL_SIZE
 * is skipped: a directory's size is its entry count, and entry
 * changes are already fully represented by the child records the
 * walker emits for every name -- counting them here again would
 * turn every parent of any change into a spurious edit.
 */
static int
rebase_sa_identity_equal(sa_handle_t *hdl_a,
    const sa_attr_type_t *tbl_a, sa_handle_t *hdl_b,
    const sa_attr_type_t *tbl_b, boolean_t isdir, boolean_t *samep)
{
	boolean_t same;
	int err;

	*samep = B_FALSE;

	for (size_t i = 0; i < sizeof (rebase_identity_fixed) /
	    sizeof (rebase_identity_fixed[0]); i++) {
		int zpl = rebase_identity_fixed[i];

		if (isdir && zpl == ZPL_SIZE)
			continue;
		err = rebase_sa_cmp_uint64(hdl_a, tbl_a[zpl],
		    hdl_b, tbl_b[zpl], &same);
		if (err != 0 || !same)
			return (err);
	}

	for (size_t i = 0; i < sizeof (rebase_identity_var) /
	    sizeof (rebase_identity_var[0]); i++) {
		int zpl = rebase_identity_var[i];

		err = rebase_sa_cmp_var(hdl_a, tbl_a[zpl],
		    hdl_b, tbl_b[zpl], &same);
		if (err != 0 || !same)
			return (err);
	}

	*samep = B_TRUE;
	return (0);
}

/*
 * Build the logical xattr set of one object as an nvlist of
 * name -> byte-array pairs, merging both physical representations:
 * SA-resident (ZPL_DXATTR, itself a packed nvlist of exactly that
 * shape) and the hidden xattr directory (ZPL_XATTR), whose entries
 * are plain file objects holding one value each. An object may
 * legitimately carry both at once -- large values overflow to the
 * directory even under xattr=sa -- which is why the two forms are
 * merged instead of chosen between.
 */
static int
rebase_xattr_set(objset_t *os, const sa_attr_type_t *tbl,
    sa_handle_t *hdl, zap_attribute_t *za, nvlist_t **setp)
{
	nvlist_t *set = fnvlist_alloc();
	uint64_t xattr_obj = 0;
	int size;
	int err;

	*setp = NULL;

	/* SA-resident form. */
	err = sa_size(hdl, tbl[ZPL_DXATTR], &size);
	if (err == 0 && size > 0) {
		nvlist_t *dx = NULL;
		char *packed = vmem_alloc(size, KM_SLEEP);

		err = sa_lookup(hdl, tbl[ZPL_DXATTR], packed, size);
		if (err == 0 && nvlist_unpack(packed, size, &dx,
		    KM_SLEEP) != 0) {
			/*
			 * An unparseable packed nvlist is corrupt
			 * on-disk input, not a caller mistake: EIO,
			 * never the unpacker's EINVAL.
			 */
			err = SET_ERROR(EIO);
		}
		vmem_free(packed, size);
		if (err == 0) {
			fnvlist_merge(set, dx);
			nvlist_free(dx);
		}
	} else if (err == ENOENT) {
		err = 0;
	}
	if (err != 0)
		goto fail;

	/* Hidden-directory form. */
	err = sa_lookup(hdl, tbl[ZPL_XATTR], &xattr_obj,
	    sizeof (xattr_obj));
	if (err == ENOENT) {
		err = 0;
		xattr_obj = 0;
	}
	if (err != 0)
		goto fail;

	if (xattr_obj != 0) {
		zap_cursor_t zc;

		for (zap_cursor_init(&zc, os, xattr_obj);
		    (err = zap_cursor_retrieve(&zc, za)) == 0;
		    zap_cursor_advance(&zc)) {
			uint64_t xobj =
			    ZFS_DIRENT_OBJ(za->za_first_integer);
			sa_handle_t *xhdl;
			uint64_t xsize;
			size_t alloc;
			void *val;

			err = sa_handle_get(os, xobj, NULL,
			    SA_HDL_PRIVATE, &xhdl);
			if (err != 0)
				break;
			err = sa_lookup(xhdl, tbl[ZPL_SIZE], &xsize,
			    sizeof (xsize));
			sa_handle_destroy(xhdl);
			if (err == ENOENT)
				err = SET_ERROR(EIO);
			if (err != 0)
				break;

			alloc = MAX(xsize, 1);
			val = vmem_alloc(alloc, KM_SLEEP);
			if (xsize > 0)
				err = dmu_read(os, xobj, 0, xsize,
				    val, DMU_READ_NO_PREFETCH);
			if (err == 0)
				fnvlist_add_byte_array(set,
				    za->za_name, (uchar_t *)val,
				    (uint_t)xsize);
			vmem_free(val, alloc);
			if (err != 0)
				break;
		}
		zap_cursor_fini(&zc);
		if (err == ENOENT)
			err = 0;
		if (err != 0)
			goto fail;
	}

	*setp = set;
	return (0);

fail:
	nvlist_free(set);
	return (err);
}

/*
 * Compare two logical xattr sets: same names, same values. Order
 * and physical representation are irrelevant by construction.
 */
static boolean_t
rebase_xattr_set_equal(nvlist_t *a, nvlist_t *b)
{
	nvpair_t *pair;
	uint_t na = 0, nb = 0;

	for (pair = nvlist_next_nvpair(a, NULL); pair != NULL;
	    pair = nvlist_next_nvpair(a, pair)) {
		uchar_t *va, *vb;
		uint_t la, lb;

		na++;
		if (nvpair_value_byte_array(pair, &va, &la) != 0)
			return (B_FALSE);
		if (nvlist_lookup_byte_array(b, nvpair_name(pair),
		    &vb, &lb) != 0)
			return (B_FALSE);
		if (la != lb || memcmp(va, vb, la) != 0)
			return (B_FALSE);
	}

	for (pair = nvlist_next_nvpair(b, NULL); pair != NULL;
	    pair = nvlist_next_nvpair(b, pair))
		nb++;

	return (na == nb);
}

/*
 * Logical xattr comparison, never representational: the same
 * logical set can live SA-resident on one side and in a hidden
 * xattr directory on the other (the xattr= property, value sizes,
 * and write history all move values between forms), so both sides
 * are unpacked to name -> value sets first. A representation flip
 * with identical logical content is hysterical.
 */
static int
rebase_xattr_equal(objset_t *os_a, const sa_attr_type_t *tbl_a,
    sa_handle_t *hdl_a, objset_t *os_b, const sa_attr_type_t *tbl_b,
    sa_handle_t *hdl_b, zap_attribute_t *za, boolean_t *samep)
{
	nvlist_t *set_a, *set_b;
	int err;

	*samep = B_FALSE;

	err = rebase_xattr_set(os_a, tbl_a, hdl_a, za, &set_a);
	if (err != 0)
		return (err);
	err = rebase_xattr_set(os_b, tbl_b, hdl_b, za, &set_b);
	if (err != 0) {
		nvlist_free(set_a);
		return (err);
	}

	*samep = rebase_xattr_set_equal(set_a, set_b);

	nvlist_free(set_b);
	nvlist_free(set_a);
	return (0);
}

/*
 * Recycling guard: the same object number in two objsets is the
 * same lineage only if ZPL_GEN matches. Object numbers are freed
 * and reused, and a reused slot is an unrelated file (ADD plus
 * DELETE), never an edit, no matter what its content says. Like
 * ZPL_LINKS, a missing ZPL_GEN on a ZPL >= 5 dataset is a hard
 * error, never a default.
 */
static int
rebase_same_gen(sa_handle_t *hdl_a, const sa_attr_type_t *tbl_a,
    sa_handle_t *hdl_b, const sa_attr_type_t *tbl_b,
    boolean_t *samep)
{
	uint64_t gen_a, gen_b;
	int err;

	*samep = B_FALSE;

	err = sa_lookup(hdl_a, tbl_a[ZPL_GEN], &gen_a,
	    sizeof (gen_a));
	if (err == 0)
		err = sa_lookup(hdl_b, tbl_b[ZPL_GEN], &gen_b,
		    sizeof (gen_b));
	if (err == ENOENT)
		return (SET_ERROR(EIO));
	if (err == 0)
		*samep = (gen_a == gen_b);
	return (err);
}

/*
 * Fork-txg fast path. An object is untouched since the fork iff
 * the dnode block holding it has a logical birth txg <=
 * rs_fork_txg: any change to the object -- data (the dn_blkptr
 * array is rewritten), spill, bonus/SA, or flags -- rewrites its
 * dnode, and dmu_diff detects object changes by exactly this
 * meta-dnode-level pruning against the from-snapshot's
 * ds_creation_txg. The object's own dn_blkptr birth txgs are
 * deliberately NOT what is walked here: they never change on a
 * bonus-only edit (chmod), so walking them would call such an
 * object untouched. Granularity is the dnode block: a neighboring
 * object's churn can only force the content tiers to run, never
 * produce a wrong answer. Both objsets are immutable snapshots
 * (the fence-post rule), so the block pointer cannot change
 * beneath the held dnode; anything unavailable degrades to
 * "touched" and the content tiers decide.
 */
static int
rebase_untouched_since_fork(objset_t *os, uint64_t obj,
    uint64_t fork_txg, boolean_t *untouchedp)
{
	dnode_t *dn;
	dmu_buf_impl_t *db;
	int err;

	*untouchedp = B_FALSE;

	err = dnode_hold(os, obj, FTAG, &dn);
	if (err != 0)
		return (err);

	db = dn->dn_dbuf;
	if (db != NULL && db->db_blkptr != NULL &&
	    !BP_IS_HOLE(db->db_blkptr) &&
	    BP_GET_LOGICAL_BIRTH(db->db_blkptr) <= fork_txg)
		*untouchedp = B_TRUE;

	dnode_rele(dn, FTAG);
	return (0);
}

/*
 * Can equal checksums on these two block pointers prove their data
 * byte-identical? Requires the same algorithm and compression on
 * both (same physical bytes then imply the same logical bytes),
 * and an algorithm strong enough that upstream trusts it for
 * NOP-writes. Fletcher does not qualify: matching fletcher
 * checksums prove nothing, and with checksum=off two never-
 * computed checksums compare equal. Either would let a real edit
 * hide behind a false "identical".
 */
static boolean_t
rebase_bp_cksum_provable(const blkptr_t *bp_a, const blkptr_t *bp_b)
{
	uint64_t alg = BP_GET_CHECKSUM(bp_a);

	return (alg == BP_GET_CHECKSUM(bp_b) &&
	    alg < ZIO_CHECKSUM_FUNCTIONS &&
	    (zio_checksum_table[alg].ci_flags &
	    ZCHECKSUM_FLAG_NOPWRITE) != 0 &&
	    BP_GET_COMPRESS(bp_a) == BP_GET_COMPRESS(bp_b));
}

/*
 * Data comparison, tiers 2 and 3. Tier 2 walks the two dnodes'
 * top-level block pointers and can only ever conclude "same" --
 * every doubtful pair (hole vs data, embedded vs external, an
 * unprovable checksum) drops to tier 3, which reads and compares
 * the logical bytes. Per top-level pair, in order: both holes are
 * equal; both embedded are equal iff the whole blkptr_t matches
 * bytewise (the payload lives in the BP words; a differing birth
 * word makes that inconclusive, not different); BP_EQUAL means the
 * same physical block, equal with no checksum trust needed (this
 * is what resolves touch(1)-style bonus churn instantly, since the
 * rewritten dnode carries its old block pointers verbatim); and
 * finally a provable checksum pair. Snapshot immutability keeps
 * dn_phys stable under the dnode hold.
 */
static int
rebase_data_equal(objset_t *os_a, uint64_t obj_a, objset_t *os_b,
    uint64_t obj_b, uint64_t size, boolean_t *samep)
{
	dmu_object_info_t doi_a, doi_b;
	boolean_t same;
	int err;

	*samep = B_FALSE;

	if (size == 0) {
		*samep = B_TRUE;
		return (0);
	}

	err = dmu_object_info(os_a, obj_a, &doi_a);
	if (err == 0)
		err = dmu_object_info(os_b, obj_b, &doi_b);
	if (err != 0)
		return (err);

	/*
	 * Tier 2 requires congruent block trees; anything else goes
	 * straight to tier 3.
	 */
	if (doi_a.doi_data_block_size == doi_b.doi_data_block_size &&
	    doi_a.doi_indirection == doi_b.doi_indirection &&
	    doi_a.doi_nblkptr == doi_b.doi_nblkptr &&
	    doi_a.doi_max_offset == doi_b.doi_max_offset) {
		dnode_t *dn_a, *dn_b;

		err = dnode_hold(os_a, obj_a, FTAG, &dn_a);
		if (err != 0)
			return (err);
		err = dnode_hold(os_b, obj_b, FTAG, &dn_b);
		if (err != 0) {
			dnode_rele(dn_a, FTAG);
			return (err);
		}

		same = B_TRUE;
		for (int i = 0; i < doi_a.doi_nblkptr; i++) {
			const blkptr_t *bp_a =
			    &dn_a->dn_phys->dn_blkptr[i];
			const blkptr_t *bp_b =
			    &dn_b->dn_phys->dn_blkptr[i];

			if (BP_IS_HOLE(bp_a) || BP_IS_HOLE(bp_b)) {
				if (BP_IS_HOLE(bp_a) &&
				    BP_IS_HOLE(bp_b))
					continue;
				same = B_FALSE;
				break;
			}
			if (BP_IS_EMBEDDED(bp_a) ||
			    BP_IS_EMBEDDED(bp_b)) {
				if (BP_IS_EMBEDDED(bp_a) &&
				    BP_IS_EMBEDDED(bp_b) &&
				    memcmp(bp_a, bp_b,
				    sizeof (blkptr_t)) == 0)
					continue;
				same = B_FALSE;
				break;
			}
			if (BP_EQUAL(bp_a, bp_b))
				continue;
			if (rebase_bp_cksum_provable(bp_a, bp_b) &&
			    ZIO_CHECKSUM_EQUAL(bp_a->blk_cksum,
			    bp_b->blk_cksum))
				continue;
			same = B_FALSE;
			break;
		}

		dnode_rele(dn_b, FTAG);
		dnode_rele(dn_a, FTAG);

		if (same) {
			*samep = B_TRUE;
			return (0);
		}
	}

	/* Tier 3: byte compare over the logical size. */
	{
		size_t bufsz = SPA_OLD_MAXBLOCKSIZE;
		char *buf_a = vmem_alloc(bufsz, KM_SLEEP);
		char *buf_b = vmem_alloc(bufsz, KM_SLEEP);
		uint64_t off = 0;

		same = B_TRUE;
		while (off < size) {
			uint64_t chunk = MIN(bufsz, size - off);

			err = dmu_read(os_a, obj_a, off, chunk,
			    buf_a, DMU_READ_NO_PREFETCH);
			if (err == 0)
				err = dmu_read(os_b, obj_b, off,
				    chunk, buf_b,
				    DMU_READ_NO_PREFETCH);
			if (err != 0)
				break;
			if (memcmp(buf_a, buf_b, chunk) != 0) {
				same = B_FALSE;
				break;
			}
			off += chunk;
		}

		vmem_free(buf_a, bufsz);
		vmem_free(buf_b, bufsz);
		if (err == 0)
			*samep = same;
	}

	return (err);
}

/*
 * Hysteria detection: does the pair (a, b) describe a
 * transformation where something LOOKS edited but nothing actually
 * changed? Slot a is base and slot b is a side when base_is_cmp
 * (the fast path and the recycling guard only make sense against
 * base); the flag exists because cross-reference will later run
 * side-vs-side comparisons through the same tiers. Both objects
 * must exist -- absent-slot cases are ADD/DELETE material and are
 * classified by standalone-diff, not here.
 *
 * Tier order: fork-txg fast path (same object untouched since the
 * fork is identical, full stop -- one integer compare; this also
 * subsumes the gen check, since recycling an object number dirties
 * its dnode block), recycling guard (same object number, touched,
 * different ZPL_GEN: an unrelated file, never an edit), SA
 * identity, logical xattrs, and only then file data. Directories
 * stop after identity and xattrs: a directory's content is its
 * entries, every entry change is already a child record in its own
 * right, and independently allocated ZAPs are not comparable
 * block-wise anyway.
 */
static int
rebase_is_hysterical(rebase_walk_ctx_t *rwc, int slot_a,
    uint64_t obj_a, int slot_b, uint64_t obj_b,
    boolean_t base_is_cmp, boolean_t *hystp)
{
	objset_t *os_a = rwc->rwc_os[slot_a];
	objset_t *os_b = rwc->rwc_os[slot_b];
	const sa_attr_type_t *tbl_a = rwc->rwc_sa[slot_a];
	const sa_attr_type_t *tbl_b = rwc->rwc_sa[slot_b];
	dmu_object_info_t doi_a, doi_b;
	sa_handle_t *hdl_a = NULL, *hdl_b = NULL;
	boolean_t isdir, same;
	uint64_t size = 0;
	int err;

	*hystp = B_FALSE;

	err = dmu_object_info(os_a, obj_a, &doi_a);
	if (err == 0)
		err = dmu_object_info(os_b, obj_b, &doi_b);
	if (err != 0)
		return (err);

	/*
	 * A directory vs non-directory flip is always a real
	 * change. (File vs symlink vs device flips fall out of the
	 * ZPL_MODE identity compare below.)
	 */
	if ((doi_a.doi_type == DMU_OT_DIRECTORY_CONTENTS) !=
	    (doi_b.doi_type == DMU_OT_DIRECTORY_CONTENTS))
		return (0);
	isdir = (doi_a.doi_type == DMU_OT_DIRECTORY_CONTENTS);

	if (base_is_cmp && obj_a == obj_b) {
		boolean_t untouched;

		err = rebase_untouched_since_fork(os_b, obj_b,
		    rwc->rwc_rs->rs_fork_txg, &untouched);
		if (err != 0)
			return (err);
		if (untouched) {
			*hystp = B_TRUE;
			return (0);
		}
	}

	err = sa_handle_get(os_a, obj_a, NULL, SA_HDL_PRIVATE,
	    &hdl_a);
	if (err != 0)
		return (err);
	err = sa_handle_get(os_b, obj_b, NULL, SA_HDL_PRIVATE,
	    &hdl_b);
	if (err != 0) {
		sa_handle_destroy(hdl_a);
		return (err);
	}

	if (base_is_cmp && obj_a == obj_b) {
		err = rebase_same_gen(hdl_a, tbl_a, hdl_b, tbl_b,
		    &same);
		if (err != 0 || !same)
			goto out;
	}

	err = rebase_sa_identity_equal(hdl_a, tbl_a, hdl_b, tbl_b,
	    isdir, &same);
	if (err != 0 || !same)
		goto out;

	err = rebase_xattr_equal(os_a, tbl_a, hdl_a, os_b, tbl_b,
	    hdl_b, rwc->rwc_za, &same);
	if (err != 0 || !same)
		goto out;

	if (!isdir) {
		err = sa_lookup(hdl_b, tbl_b[ZPL_SIZE], &size,
		    sizeof (size));
		if (err == ENOENT)
			err = SET_ERROR(EIO);
		if (err != 0)
			goto out;
	}

	sa_handle_destroy(hdl_b);
	sa_handle_destroy(hdl_a);

	if (isdir) {
		*hystp = B_TRUE;
		return (0);
	}

	return (rebase_data_equal(os_a, obj_a, os_b, obj_b, size,
	    hystp));

out:
	sa_handle_destroy(hdl_b);
	sa_handle_destroy(hdl_a);
	return (err);
}

/*
 * Per-path three-slot diff analysis: the left, base, and right
 * objects visible at one path (0 = absent on that side). This
 * issue computes each side's hysteria status against base and the
 * per-slot linkpool participation; standalone-diff consumes both
 * to build the two-axis change records. Until it lands the results
 * are only counted (and reported through dbgmsg at the end of the
 * walk) so the machinery runs end to end, and the overall
 * operation still exits with ENOSYS.
 */
static int
rebase_walk_diff(rebase_walk_ctx_t *rwc, const char *path,
    size_t pathlen, uint64_t left_obj, uint64_t base_obj,
    uint64_t right_obj)
{
	rebase_state_t *rs = rwc->rwc_rs;
	boolean_t hyst;
	int err;

	(void) pathlen;

	rwc->rwc_nvisited++;

	if (base_obj != 0 && left_obj != 0) {
		err = rebase_is_hysterical(rwc, REBASE_WALK_BASE,
		    base_obj, REBASE_WALK_LEFT, left_obj, B_TRUE,
		    &hyst);
		if (err != 0)
			return (err);
		if (hyst)
			rwc->rwc_nhysterical_left++;
	}

	if (base_obj != 0 && right_obj != 0) {
		err = rebase_is_hysterical(rwc, REBASE_WALK_BASE,
		    base_obj, REBASE_WALK_RIGHT, right_obj, B_TRUE,
		    &hyst);
		if (err != 0)
			return (err);
		if (hyst)
			rwc->rwc_nhysterical_right++;
	}

	/*
	 * Linkpool participation per slot, by path. The walker
	 * records a path's own membership before calling here, so a
	 * self-lookup is complete even though the tables are still
	 * being built.
	 */
	if (rebase_linkpool_of(&rs->rs_left_linkpools, path) != NULL ||
	    rebase_linkpool_of(&rs->rs_base_linkpools, path) != NULL ||
	    rebase_linkpool_of(&rs->rs_right_linkpools, path) != NULL)
		rwc->rwc_nlinked++;

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
	rebase_linkpool_table_t *rlpts[REBASE_WALK_NSLOTS];
	uint64_t objs[REBASE_WALK_NSLOTS];
	boolean_t isdir[REBASE_WALK_NSLOTS];
	char *cpath;
	size_t cpathlen;
	int err = 0;

	rlpts[REBASE_WALK_LEFT] = &rs->rs_left_linkpools;
	rlpts[REBASE_WALK_BASE] = &rs->rs_base_linkpools;
	rlpts[REBASE_WALK_RIGHT] = &rs->rs_right_linkpools;
	objs[REBASE_WALK_LEFT] = left_obj;
	objs[REBASE_WALK_BASE] = base_obj;
	objs[REBASE_WALK_RIGHT] = right_obj;

	cpath = rebase_build_path(parent, parentlen, name, &cpathlen);

	for (int i = 0; i < REBASE_WALK_NSLOTS; i++) {
		dmu_object_info_t doi;
		uint64_t nlink;

		isdir[i] = B_FALSE;
		if (objs[i] == 0)
			continue;

		err = dmu_object_info(rwc->rwc_os[i], objs[i], &doi);
		if (err != 0)
			goto out;

		if (doi.doi_type == DMU_OT_DIRECTORY_CONTENTS) {
			isdir[i] = B_TRUE;
			continue;
		}

		err = rebase_get_nlink(rwc->rwc_os[i], rwc->rwc_sa[i],
		    objs[i], &nlink);
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
	/*
	 * Every object number that reaches this function came from a
	 * live directory entry, so ENOENT from any layer below means
	 * a dangling reference -- on-disk corruption, reported as
	 * EIO like every other corruption the walk detects. It must
	 * also never leak upward as ENOENT: the phase loops in
	 * rebase_walk_dir normalize ENOENT as end-of-cursor and
	 * would silently swallow it, and dsl_rebase's callers read
	 * ENOENT as "no common ancestor".
	 */
	if (err == ENOENT) {
		zfs_dbgmsg("rebase: dangling reference at %s", cpath);
		err = SET_ERROR(EIO);
	}
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

	memset(&rwc, 0, sizeof (rwc));
	rwc.rwc_rs = rs;
	rwc.rwc_os[REBASE_WALK_LEFT] = rs->rs_left_os;
	rwc.rwc_os[REBASE_WALK_BASE] = rs->rs_base_os;
	rwc.rwc_os[REBASE_WALK_RIGHT] = rs->rs_right_os;

	err = rebase_sa_setup(rs->rs_left_os,
	    &rwc.rwc_sa[REBASE_WALK_LEFT]);
	if (err == 0)
		err = rebase_sa_setup(rs->rs_base_os,
		    &rwc.rwc_sa[REBASE_WALK_BASE]);
	if (err == 0)
		err = rebase_sa_setup(rs->rs_right_os,
		    &rwc.rwc_sa[REBASE_WALK_RIGHT]);
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

	if (err == 0)
		zfs_dbgmsg("rebase: walk visited %llu paths, "
		    "hysterical left %llu right %llu, "
		    "linkpool-member paths %llu",
		    (u_longlong_t)rwc.rwc_nvisited,
		    (u_longlong_t)rwc.rwc_nhysterical_left,
		    (u_longlong_t)rwc.rwc_nhysterical_right,
		    (u_longlong_t)rwc.rwc_nlinked);

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

	/*
	 * Every hold is in hand and every objset pointer is
	 * materialized: switch to long holds and drop the pool
	 * config lock before the long-running phases. From here the
	 * cleanup path is the long_rele ladder below, which never
	 * touches the pool.
	 */
	rebase_long_hold(dp, &state, left_snap, right_snap);

	err = rebase_state_setup(&state, left_snap_os, right_snap_os);
	if (err != 0)
		goto long_rele;

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
long_rele:
	rebase_long_rele(&state, left_snap, right_snap);
	if (right_snap != NULL)
		dsl_dataset_rele(right_snap, FTAG);
	dsl_dataset_rele(left_snap, FTAG);
	dsl_dataset_rele(state.rs_base, &state);
	dsl_dataset_rele(state.rs_right, &state);
	dsl_dataset_rele(state.rs_left, &state);
	goto destroy_snaps;

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
