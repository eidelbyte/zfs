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

#include <sys/dsl_rebase.h>
#include <sys/dsl_dir.h>
#include <sys/dmu.h>

/*
 * Collect all snapshot object IDs in the ancestry chain of a dataset.
 *
 * Starting from the head dataset, walks ds_prev_snap_obj through the
 * entire snapshot chain. This naturally crosses clone boundaries -- a
 * clone's ds_prev_snap_obj chain includes the origin snapshot and
 * continues into the origin's own ancestor snapshots.
 *
 * Returns an allocated array of object IDs (most-recent-first) in
 * *objsp, and the count in *countp. Caller must
 * kmem_free(*objsp, *countp * sizeof (uint64_t)).
 */
static int
dsl_rebase_collect_snaps(dsl_pool_t *dp, dsl_dataset_t *ds,
    uint64_t **objsp, uint_t *countp, const void *tag)
{
	uint_t alloc = 64;
	uint_t count = 0;
	uint64_t *objs = kmem_alloc(alloc * sizeof (uint64_t), KM_SLEEP);
	uint64_t obj;
	int err = 0;

	/*
	 * Start from the dataset's most recent snapshot. For a head dataset,
	 * that's ds_prev_snap_obj. For a snapshot, it's the snapshot itself.
	 */
	if (ds->ds_is_snapshot) {
		obj = ds->ds_object;
	} else {
		obj = dsl_dataset_phys(ds)->ds_prev_snap_obj;
	}

	while (obj != 0) {
		dsl_dataset_t *snap;

		err = dsl_dataset_hold_obj(dp, obj, tag, &snap);
		if (err != 0)
			break;

		if (count == alloc) {
			uint_t newalloc = alloc * 2;
			uint64_t *newobjs = kmem_alloc(
			    newalloc * sizeof (uint64_t), KM_SLEEP);
			memcpy(newobjs, objs, alloc * sizeof (uint64_t));
			kmem_free(objs, alloc * sizeof (uint64_t));
			objs = newobjs;
			alloc = newalloc;
		}
		objs[count++] = obj;

		uint64_t prev = dsl_dataset_phys(snap)->ds_prev_snap_obj;
		dsl_dataset_rele(snap, tag);
		obj = prev;
	}

	if (err != 0) {
		kmem_free(objs, alloc * sizeof (uint64_t));
		return (err);
	}

	if (count == 0) {
		kmem_free(objs, alloc * sizeof (uint64_t));
		*objsp = NULL;
		*countp = 0;
		return (0);
	}

	/* Shrink to exact size so caller can kmem_free with count */
	if (count < alloc) {
		uint64_t *exact = kmem_alloc(count * sizeof (uint64_t),
		    KM_SLEEP);
		memcpy(exact, objs, count * sizeof (uint64_t));
		kmem_free(objs, alloc * sizeof (uint64_t));
		objs = exact;
	}

	*objsp = objs;
	*countp = count;
	return (0);
}

int
dsl_rebase_find_ancestor(dsl_pool_t *dp, dsl_dataset_t *base,
    dsl_dataset_t *after, const void *tag, dsl_dataset_t **ancestor)
{
	uint64_t *base_snaps = NULL;
	uint_t base_count = 0;
	uint64_t *after_snaps = NULL;
	uint_t after_count = 0;
	int err;

	*ancestor = NULL;

	err = dsl_rebase_collect_snaps(dp, base, &base_snaps, &base_count,
	    tag);
	if (err != 0)
		return (err);

	err = dsl_rebase_collect_snaps(dp, after, &after_snaps, &after_count,
	    tag);
	if (err != 0) {
		if (base_snaps != NULL)
			kmem_free(base_snaps,
			    base_count * sizeof (uint64_t));
		return (err);
	}

	/*
	 * Find the most recent snapshot present in both chains. The
	 * after chain is ordered most-recent-first, so the first match
	 * we find walking the after chain is the most recent common
	 * ancestor.
	 */
	uint64_t found_obj = 0;
	for (uint_t i = 0; i < after_count && found_obj == 0; i++) {
		for (uint_t j = 0; j < base_count; j++) {
			if (after_snaps[i] == base_snaps[j]) {
				found_obj = after_snaps[i];
				break;
			}
		}
	}

	if (base_snaps != NULL)
		kmem_free(base_snaps, base_count * sizeof (uint64_t));
	if (after_snaps != NULL)
		kmem_free(after_snaps, after_count * sizeof (uint64_t));

	if (found_obj == 0)
		return (SET_ERROR(ENOENT));

	return (dsl_dataset_hold_obj(dp, found_obj, tag, ancestor));
}
