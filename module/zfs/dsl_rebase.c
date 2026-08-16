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
#include <sys/dmu_traverse.h>
#include <sys/dmu_objset.h>
#include <sys/avl.h>
#include <sys/zio.h>

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

/*
 * AVL node for collecting unique dnode object IDs during traversal.
 */
typedef struct rebase_obj_node {
	uint64_t	ron_obj;
	avl_node_t	ron_avl;
} rebase_obj_node_t;

typedef struct rebase_enum_arg {
	avl_tree_t	re_tree;
} rebase_enum_arg_t;

static int
rebase_obj_compare(const void *a, const void *b)
{
	const rebase_obj_node_t *ra = (const rebase_obj_node_t *)a;
	const rebase_obj_node_t *rb = (const rebase_obj_node_t *)b;
	return (TREE_CMP(ra->ron_obj, rb->ron_obj));
}

/*
 * traverse_dataset callback: record each modified dnode object ID.
 */
static int
rebase_enum_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const struct dnode_phys *dnp, void *arg)
{
	(void) spa;
	(void) zilog;
	(void) bp;
	(void) dnp;
	rebase_enum_arg_t *rea = arg;
	avl_index_t where;

	if (zb->zb_level != ZB_DNODE_LEVEL)
		return (0);
	if (zb->zb_object == DMU_META_DNODE_OBJECT ||
	    DMU_OBJECT_IS_SPECIAL(zb->zb_object))
		return (0);

	rebase_obj_node_t search = { .ron_obj = zb->zb_object };
	if (avl_find(&rea->re_tree, &search, &where) != NULL)
		return (0);

	rebase_obj_node_t *node = kmem_alloc(sizeof (*node), KM_SLEEP);
	node->ron_obj = zb->zb_object;
	avl_insert(&rea->re_tree, node, where);

	return (0);
}

/*
 * Traverse a snapshot and collect the object IDs of all dnodes modified
 * after from_txg.  Returns a sorted array; caller must
 * kmem_free(*objsp, *countp * sizeof (uint64_t)).
 */
static int
dsl_rebase_collect_changed_objs(dsl_dataset_t *ds, uint64_t from_txg,
    uint64_t **objsp, uint_t *countp)
{
	rebase_enum_arg_t rea;
	int err;

	avl_create(&rea.re_tree, rebase_obj_compare,
	    sizeof (rebase_obj_node_t),
	    offsetof(rebase_obj_node_t, ron_avl));

	err = traverse_dataset(ds, from_txg,
	    TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA, rebase_enum_cb, &rea);

	uint_t count = (uint_t)avl_numnodes(&rea.re_tree);
	uint64_t *objs = NULL;

	if (err == 0 && count > 0) {
		objs = kmem_alloc(count * sizeof (uint64_t), KM_SLEEP);
		uint_t i = 0;
		rebase_obj_node_t *node;
		for (node = avl_first(&rea.re_tree); node != NULL;
		    node = AVL_NEXT(&rea.re_tree, node)) {
			objs[i++] = node->ron_obj;
		}
		ASSERT3U(i, ==, count);
	}

	void *cookie = NULL;
	rebase_obj_node_t *node;
	while ((node = avl_destroy_nodes(&rea.re_tree, &cookie)) != NULL)
		kmem_free(node, sizeof (*node));
	avl_destroy(&rea.re_tree);

	if (err != 0)
		return (err);

	*objsp = objs;
	*countp = count;
	return (0);
}

int
dsl_rebase_enum_deltas(dsl_pool_t *dp, dsl_dataset_t *base,
    dsl_dataset_t *after, const void *tag,
    dsl_dataset_t **ancestor,
    uint64_t **base_objsp, uint_t *base_countp,
    uint64_t **after_objsp, uint_t *after_countp)
{
	uint64_t from_txg;
	int err;

	*base_objsp = NULL;
	*base_countp = 0;
	*after_objsp = NULL;
	*after_countp = 0;

	err = dsl_rebase_find_ancestor(dp, base, after, tag, ancestor);
	if (err != 0)
		return (err);

	/*
	 * Blocks born at ds_creation_txg belong to the ancestor itself.
	 * We want blocks born after the ancestor was taken.
	 */
	from_txg = dsl_dataset_phys(*ancestor)->ds_creation_txg + 1;

	err = dsl_rebase_collect_changed_objs(base, from_txg, base_objsp,
	    base_countp);
	if (err != 0) {
		dsl_dataset_rele(*ancestor, tag);
		*ancestor = NULL;
		return (err);
	}

	err = dsl_rebase_collect_changed_objs(after, from_txg, after_objsp,
	    after_countp);
	if (err != 0) {
		if (*base_objsp != NULL)
			kmem_free(*base_objsp,
			    *base_countp * sizeof (uint64_t));
		*base_objsp = NULL;
		*base_countp = 0;
		dsl_dataset_rele(*ancestor, tag);
		*ancestor = NULL;
		return (err);
	}

	return (0);
}
