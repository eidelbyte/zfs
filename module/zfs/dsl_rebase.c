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

int
dsl_rebase(const char *left_ds, const char *right_ds, nvlist_t *outnvl)
{
	dsl_pool_t *dp;
	dsl_dataset_t *left, *right, *base;
	objset_t *left_os, *right_os, *base_os;
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

	/* Run all precondition checks. */
	err = rebase_check_preconditions(dp, left, right,
	    left_os, right_os, base_os);
	if (err != 0)
		goto rele_base;

	/*
	 * Preconditions passed.  Subsequent issues fill in the
	 * dataset-setup, diff, apply, and emit phases here.
	 */
	err = SET_ERROR(ENOSYS);

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
