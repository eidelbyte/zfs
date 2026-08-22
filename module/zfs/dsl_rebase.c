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
#include <sys/zap.h>
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

	/*
	 * State initialized.  Subsequent issues fill in the diff,
	 * cross-reference, emit, and apply phases here.
	 */
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
