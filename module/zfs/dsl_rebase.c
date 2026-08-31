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
#include <sys/dsl_pool.h>
#include <sys/dmu.h>
#include <sys/spa.h>
#include <sys/nvpair.h>

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

int
dsl_rebase(const char *left_ds, const char *right_ds, nvlist_t *outnvl)
{
	dsl_pool_t *dp;
	dsl_dataset_t *left, *right, *base;
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
	if (left->ds_is_snapshot) {
		dsl_dataset_rele(left, FTAG);
		dsl_pool_rele(dp, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Right may be a dataset (head) or a snapshot.
	 * dsl_dataset_hold handles both -- '@' in the name
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
	    !right->ds_is_snapshot) {
		err = SET_ERROR(EINVAL);
		goto rele_both;
	}

	/* Find the common ancestor snapshot. */
	err = rebase_find_common(dp, left, right, FTAG, &base);
	if (err != 0)
		goto rele_both;

	/*
	 * Common ancestor found and held.  Subsequent issues add
	 * preconditions, fence posts and long holds, the walk, the
	 * decide passes, emission, and the manifest.
	 */
	err = SET_ERROR(ENOSYS);

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
