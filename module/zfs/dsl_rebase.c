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
 * ZFS rebase -- a git-rebase-like three-way merge for ZFS datasets,
 * revision 3: the graph-theoretic decide engine.
 *
 * Given two datasets -- left and right -- auto-discover their common
 * ancestor snapshot, build the three trees as pool sets (a pool is
 * one dnode plus every name reaching it), and decide the merge on
 * the evidence graph: green edges for shared names, red edges for
 * dnode identity, the yellow relation for content.  Conflicts carry
 * machine-readable certificates; components a conflict touches are
 * quarantined; the rest compiles to a dependency-ordered edit list.
 *
 * This translation unit owns the driver: ancestor discovery,
 * preconditions, holds and fence-post snapshots, the per-tree walk,
 * and the ioctl surface.  The decide passes live in
 * dsl_rebase_decide.c, edit compilation and the manifest in
 * dsl_rebase_emit.c, and the apply primitives in dsl_rebase_apply.c.
 * The type contract and the model overview live in sys/dsl_rebase.h.
 */

#include <sys/zfs_context.h>
#include <sys/dsl_rebase.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_destroy.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_scan.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/spa.h>
#include <sys/nvpair.h>
#include <sys/zap.h>
#include <sys/zfs_acl.h>	/* zfs_sa.h needs zfs_acl_phys_t */
#include <sys/zfs_sa.h>
#include <sys/zfs_znode.h>

/*
 * Memory cap for one decide run's arena, in bytes; 0 means the
 * built-in default.  Enforced at the arena allocator, the single
 * enforcement point.  Promoted to a module parameter when the arena
 * lands (dataset-setup-v3).
 */
uint64_t rebase_mem_limit_bytes = 0;

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
 * must release it.  A hold failure mid-walk propagates as its own
 * error: it must never collapse into ENOENT, which callers read as
 * "no common ancestor" (revision 2 swallowed it; fixed here).
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
		if (err != 0) {
			rebase_snap_tree_destroy(&left_snaps);
			return (err);
		}

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

static int
rebase_master_lookup(objset_t *os, const char *key, uint64_t *valp)
{
	return (zap_lookup(os, MASTER_NODE_OBJ, key, 8, 1, valp));
}

/*
 * The ZPL properties that define name semantics. If any of these
 * differ between the three datasets, "same name" is not
 * well-defined and no phase of the engine may run.
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
 *   2. All three objsets are ZPL filesystems -- everything below
 *      reads MASTER_NODE, and a zvol argument would trip debug
 *      assertions before returning cleanly
 *   3. No scrub or resilver engaged
 *   4. Encryption compatibility (both sides encrypted or both not)
 *   5. ZPL version >= 5 on all three (SA layout required)
 *   6. FUID table object identical across all three
 *   7. Name semantics (casesensitivity, normalization, utf8only)
 *      equal across all three, and restricted to casesensitivity =
 *      sensitive with normalization = none: the engine matches
 *      names byte-exactly, which is only correct when every name
 *      has exactly one stored form
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
 * Set up SA for an objset and return its attribute table. Nothing
 * has mounted these objsets (base and the fences are snapshots, a
 * right given as a snapshot likewise), so the ZPL has not registered
 * the attribute table for us.
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
 * The per-run arena.  Every decide-side allocation goes through it,
 * so the memory cap has exactly one enforcement point and teardown
 * is one sweep; and because nothing lives in globals, two runs can
 * coexist (the polarity mirror check runs the engine twice).  The
 * allocation function itself lands with its first caller, the walk.
 */
#define	REBASE_MEM_LIMIT_DEFAULT	(1ULL << 30)

typedef struct rebase_arena_block {
	list_node_t	rab_node;
	size_t		rab_size;	/* whole block, header included */
} rebase_arena_block_t;

static void
rebase_arena_init(rebase_arena_t *ra)
{
	list_create(&ra->ra_blocks, sizeof (rebase_arena_block_t),
	    offsetof(rebase_arena_block_t, rab_node));
	ra->ra_used = 0;
	ra->ra_limit = rebase_mem_limit_bytes != 0 ?
	    rebase_mem_limit_bytes : REBASE_MEM_LIMIT_DEFAULT;
}

static void
rebase_arena_fini(rebase_arena_t *ra)
{
	rebase_arena_block_t *rab;

	while ((rab = list_remove_head(&ra->ra_blocks)) != NULL)
		vmem_free(rab, rab->rab_size);
	list_destroy(&ra->ra_blocks);
	ra->ra_used = 0;
}

/*
 * The dataset named as right, wherever it is held: rr_right_head
 * when right arrived as a live head, otherwise the snapshot sitting
 * in the onto read-source slot.
 */
static dsl_dataset_t *
rebase_right_named(rebase_run_t *rr)
{
	return (rr->rr_right_head != NULL ? rr->rr_right_head :
	    rr->rr_ds[REBASE_TIDX(REBASE_TREE_ONTO)]);
}

/*
 * Hold what both passes need: the pool, the left head (the apply
 * target), the named right, and the discovered common ancestor.
 * The run address is the hold tag throughout, so releases must
 * receive the same run.  On error, nothing is left held.
 */
static int
rebase_run_hold(const char *left_ds, const char *right_ds,
    dsl_pool_t **dpp, rebase_run_t *rr)
{
	dsl_pool_t *dp;
	dsl_dataset_t *right;
	int err;

	err = dsl_pool_hold(left_ds, rr, &dp);
	if (err != 0)
		return (err);

	/* Left must be a dataset (head), never a snapshot. */
	err = dsl_dataset_hold(dp, left_ds, rr, &rr->rr_left_head);
	if (err != 0)
		goto rele_pool;
	if (rr->rr_left_head->ds_is_snapshot) {
		err = SET_ERROR(EINVAL);
		goto rele_left;
	}

	/*
	 * Right may be a dataset (head) or a snapshot.
	 * dsl_dataset_hold handles both -- '@' in the name
	 * selects the snapshot.
	 */
	err = dsl_dataset_hold(dp, right_ds, rr, &right);
	if (err != 0)
		goto rele_left;

	/* Left and right must be different datasets. */
	if (dsl_dataset_phys(rr->rr_left_head)->ds_dir_obj ==
	    dsl_dataset_phys(right)->ds_dir_obj &&
	    !right->ds_is_snapshot) {
		err = SET_ERROR(EINVAL);
		goto rele_right;
	}

	err = rebase_find_common(dp, rr->rr_left_head, right, rr,
	    &rr->rr_ds[REBASE_TIDX(REBASE_TREE_BASE)]);
	if (err != 0)
		goto rele_right;

	/*
	 * A right given as a snapshot is already immutable and IS
	 * the onto read source; a live right head is held on its
	 * own and fenced later.
	 */
	if (right->ds_is_snapshot) {
		rr->rr_ds[REBASE_TIDX(REBASE_TREE_ONTO)] = right;
		rr->rr_right_head = NULL;
	} else {
		rr->rr_right_head = right;
		rr->rr_ds[REBASE_TIDX(REBASE_TREE_ONTO)] = NULL;
	}
	rr->rr_dp = dp;
	*dpp = dp;
	return (0);

rele_right:
	dsl_dataset_rele(right, rr);
rele_left:
	dsl_dataset_rele(rr->rr_left_head, rr);
rele_pool:
	dsl_pool_rele(dp, rr);
	return (err);
}

/*
 * Release the pass-1 holds (or the pass-2 holds before the fences
 * were taken).  The off-of slot is empty at these stages.
 */
static void
rebase_run_rele(dsl_pool_t *dp, rebase_run_t *rr)
{
	dsl_dataset_rele(rr->rr_ds[REBASE_TIDX(REBASE_TREE_BASE)], rr);
	dsl_dataset_rele(rebase_right_named(rr), rr);
	dsl_dataset_rele(rr->rr_left_head, rr);
	dsl_pool_rele(dp, rr);
	rr->rr_dp = NULL;
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
 * running rebase wants.  By this point every read-source slot is
 * populated (the fences included), so the loop covers them all.
 */
static void
rebase_long_hold(dsl_pool_t *dp, rebase_run_t *rr)
{
	dsl_dataset_long_hold(rr->rr_left_head, rr);
	if (rr->rr_right_head != NULL)
		dsl_dataset_long_hold(rr->rr_right_head, rr);
	for (int i = 0; i < REBASE_TREES; i++)
		dsl_dataset_long_hold(rr->rr_ds[i], rr);
	dsl_pool_rele(dp, rr);
	rr->rr_dp = NULL;
}

/*
 * Drop the long holds placed by rebase_long_hold(). The plain
 * holds remain and are released by the caller afterward -- legal
 * without the pool lock, per the same formula. The fence-post
 * snapshots must be fully released before the destroy-on-exit
 * sync task runs, or it would see its own EBUSY.
 */
static void
rebase_long_rele(rebase_run_t *rr)
{
	for (int i = REBASE_TREES - 1; i >= 0; i--)
		dsl_dataset_long_rele(rr->rr_ds[i], rr);
	if (rr->rr_right_head != NULL)
		dsl_dataset_long_rele(rr->rr_right_head, rr);
	dsl_dataset_long_rele(rr->rr_left_head, rr);
}

/*
 * The precondition sweep over the held run: derive the three
 * borrowed objsets (the named datasets, not the fences -- in pass 1
 * the fences do not exist yet, and the checks read properties the
 * fences share with their heads) and run the checks.
 */
static int
rebase_validate(dsl_pool_t *dp, rebase_run_t *rr)
{
	objset_t *los, *ros, *bos;
	int err;

	err = dmu_objset_from_ds(rr->rr_left_head, &los);
	if (err == 0)
		err = dmu_objset_from_ds(rebase_right_named(rr), &ros);
	if (err == 0)
		err = dmu_objset_from_ds(
		    rr->rr_ds[REBASE_TIDX(REBASE_TREE_BASE)], &bos);
	if (err == 0)
		err = rebase_check_preconditions(dp, rr->rr_left_head,
		    rebase_right_named(rr), los, ros, bos);
	return (err);
}

/*
 * Populate the run after the long holds are in place: per tree, the
 * borrowed objset, the root directory object, and the SA attribute
 * table (per objset, never shared -- attribute numbers are
 * registered per objset); the fork txg; and the arena.
 */
static int
rebase_run_setup(rebase_run_t *rr)
{
	static const rebase_tree_tag_t tags[REBASE_TREES] = {
		REBASE_TREE_BASE, REBASE_TREE_ONTO, REBASE_TREE_OFFOF
	};
	int err;

	/*
	 * The arena comes up first so the teardown path may always
	 * run rebase_arena_fini, even when a later step here fails.
	 */
	rebase_arena_init(&rr->rr_arena);

	for (int i = 0; i < REBASE_TREES; i++) {
		rebase_tree_t *rt = &rr->rr_trees[i];

		rt->rt_tag = tags[i];
		err = dmu_objset_from_ds(rr->rr_ds[i], &rt->rt_os);
		if (err != 0)
			return (err);
		err = rebase_master_lookup(rt->rt_os, ZFS_ROOT_OBJ,
		    &rt->rt_root_obj);
		if (err != 0)
			return (err);
		err = rebase_sa_setup(rt->rt_os, &rt->rt_sa_table);
		if (err != 0)
			return (err);
	}

	/*
	 * The fork point: the common ancestor's creation txg. A
	 * dnode whose logical birth is <= rr_fork_txg is untouched
	 * since the fork. The sides allocate from the shared SPA
	 * counter concurrently after the fork, so txgs are only
	 * ever compared against rr_fork_txg, never across sides.
	 */
	rr->rr_fork_txg = dsl_dataset_phys(
	    rr->rr_ds[REBASE_TIDX(REBASE_TREE_BASE)])->ds_creation_txg;

	return (0);
}

static void
rebase_run_teardown(rebase_run_t *rr)
{
	rebase_arena_fini(&rr->rr_arena);
}

int
dsl_rebase(const char *left_ds, const char *right_ds, nvlist_t *outnvl)
{
	dsl_pool_t *dp;
	rebase_run_t run;
	char *snapname, *right_snapname;
	boolean_t right_is_head;
	int err;

	(void) outnvl;

	memset(&run, 0, sizeof (run));
	right_snapname = NULL;

	/*
	 * Pass 1 -- validate. The fence-post snapshots may only be
	 * created once preconditions pass, but snapshot creation is
	 * a sync task and would deadlock against this thread's own
	 * pool hold, so validation is a complete hold/rele cycle of
	 * its own.
	 */
	err = rebase_run_hold(left_ds, right_ds, &dp, &run);
	if (err != 0)
		return (err);
	err = rebase_validate(dp, &run);
	right_is_head = (run.rr_right_head != NULL);
	rebase_run_rele(dp, &run);
	if (err != 0)
		return (err);

	/*
	 * Fence-post snapshots. Creation commits the ZIL, so walked
	 * on-disk state equals logical state; the engine never walks
	 * a live objset. The left snapshot is the read source for
	 * every off-of access and, once the apply phase exists, the
	 * universal rollback target. A right side given as a live
	 * head gets the same fence; a right given as a snapshot is
	 * already immutable. EEXIST here means a previous rebase
	 * left its snapshot behind.
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
	 * take the fences as the read sources.
	 */
	memset(&run, 0, sizeof (run));
	err = rebase_run_hold(left_ds, right_ds, &dp, &run);
	if (err != 0)
		goto destroy_snaps;
	err = rebase_validate(dp, &run);
	if (err != 0)
		goto rele;
	run.rr_left_fence_created = B_TRUE;
	run.rr_right_fence_created = right_is_head;

	err = dsl_dataset_hold(dp, snapname, &run,
	    &run.rr_ds[REBASE_TIDX(REBASE_TREE_OFFOF)]);
	if (err != 0)
		goto rele;
	if (right_is_head) {
		err = dsl_dataset_hold(dp, right_snapname, &run,
		    &run.rr_ds[REBASE_TIDX(REBASE_TREE_ONTO)]);
		if (err != 0)
			goto rele_offof;
	}

	/*
	 * Every hold is in hand: switch to long holds and drop the
	 * pool config lock before the long-running work. From here
	 * the cleanup path is the teardown ladder below, which
	 * never touches the pool.
	 */
	rebase_long_hold(dp, &run);

	err = rebase_run_setup(&run);
	if (err != 0)
		goto teardown;

	/*
	 * The substrate is ready: three immutable read sources with
	 * objsets, roots, and SA tables, the fork txg, and the
	 * arena. The walk lands next; ENOSYS is the success signal
	 * of the scaffolding era.
	 */
	err = SET_ERROR(ENOSYS);

teardown:
	rebase_run_teardown(&run);
	rebase_long_rele(&run);
	for (int i = REBASE_TREES - 1; i >= 0; i--)
		dsl_dataset_rele(run.rr_ds[i], &run);
	if (run.rr_right_head != NULL)
		dsl_dataset_rele(run.rr_right_head, &run);
	dsl_dataset_rele(run.rr_left_head, &run);
	goto destroy_snaps;

rele_offof:
	dsl_dataset_rele(run.rr_ds[REBASE_TIDX(REBASE_TREE_OFFOF)],
	    &run);
rele:
	rebase_run_rele(dp, &run);
destroy_snaps:
	/*
	 * Pre-apply era: both fences are destroyed on every exit.
	 * With no apply there is nothing to roll back to, and a
	 * stranded fence would EEXIST the next run. The
	 * fence-survives rule (the rollback anchor and in-progress
	 * marker of the user's 2026-08-22 decision) returns with
	 * the apply-driver issue, which owns that lifecycle.
	 * Rollback and destruction are sync tasks: no holds remain.
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
