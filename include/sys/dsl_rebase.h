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
 * Copyright (c) 2026, Ellie Skinner. All rights reserved.
 */

#ifndef	_SYS_DSL_REBASE_H
#define	_SYS_DSL_REBASE_H

#include <sys/dmu.h>
#include <sys/zfs_context.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct dsl_dataset;

/*
 * Per-entry change types. Describes what happened to a single
 * file/dir/symlink/device between the base snapshot (A) and one
 * side's HEAD (C or X).
 */
typedef enum rebase_change_type {
	RCT_ADD,		/* entry created			*/
	RCT_DELETE,		/* entry removed			*/
	RCT_EDIT,		/* content and/or metadata changed	*/
	RCT_MOVE,		/* renamed/moved, content unchanged	*/
	RCT_MOVE_EDIT,		/* renamed/moved and content changed	*/
	RCT_HARDLINK_ADD,	/* new dir entry for existing dnode	*/
	RCT_HARDLINK_DELETE	/* dir entry removed, dnode persists	*/
} rebase_change_type_t;

/*
 * A single change record. Appears in both AVL trees of its
 * parent changelist (indexed by path and by object number).
 */
typedef struct rebase_change {
	rebase_change_type_t	rc_type;
	uint64_t		rc_obj;		/* dnode object number	*/
	char			*rc_path;	/* full resolved path	*/
	size_t			rc_pathlen;	/* for kmem_free	*/

	/* MOVE / MOVE_EDIT only */
	char			*rc_old_path;	/* pre-move path	*/
	size_t			rc_old_pathlen;

	/* xattr=dir satellite tracking */
	uint64_t		rc_xattr_obj;	/* hidden dir obj (0=none) */
	boolean_t		rc_xattr_changed; /* differs from base	*/

	uint8_t			rc_dn_type;	/* DMU object type	*/

	avl_node_t		rc_avl_path;	/* rcl_by_path index	*/
	avl_node_t		rc_avl_obj;	/* rcl_by_obj index	*/
} rebase_change_t;

/*
 * Per-side change list. Two AVL indices over the same set of
 * rebase_change_t nodes:
 *   by_path  — for cross-referencing left vs right (conflict detection)
 *   by_obj   — for move/hardlink collapse (finding duplicate dnode indices)
 */
typedef struct rebase_changelist {
	avl_tree_t	rcl_by_path;
	avl_tree_t	rcl_by_obj;
	uint_t		rcl_count;
} rebase_changelist_t;

/*
 * Conflict types detected during cross-referencing.
 */
typedef enum rebase_conflict_type {
	RCONF_BOTH_MODIFIED,		/* same path: EDIT on both sides */
	RCONF_CREATE_CREATE,		/* same path: ADD on both sides	*/
	RCONF_MODIFY_DELETE,		/* left EDIT, right DELETE	*/
	RCONF_DELETE_MODIFY,		/* left DELETE, right EDIT	*/
	RCONF_MOVE_DIVERGE,		/* same dnode MOVEd differently	*/
	RCONF_MOVE_VS_EDIT,		/* one MOVE, other EDIT/DELETE	*/
	RCONF_DIR_DELETE_VS_EDIT	/* dir deleted vs contents edited */
} rebase_conflict_type_t;

/*
 * A single conflict record.
 */
typedef struct rebase_conflict {
	rebase_conflict_type_t	rcf_type;
	uint64_t		rcf_obj;
	char			*rcf_path;
	size_t			rcf_pathlen;

	/* hardlinked conflicts: other paths to same dnode */
	char			**rcf_alt_paths;
	uint_t			rcf_nalt;

	list_node_t		rcf_node;	/* in rm_conflicts	*/
} rebase_conflict_t;

/*
 * Conflict manifest — the output of the cross-reference phase.
 */
typedef struct rebase_manifest {
	list_t		rm_conflicts;		/* list of rebase_conflict_t */
	uint_t		rm_nconflicts;
} rebase_manifest_t;

/*
 * Top-level rebase operation state. Allocated at the start of
 * dsl_rebase() and torn down on completion or error.
 */
typedef struct rebase_state {
	/* input datasets (held) */
	struct dsl_dataset	*rs_left;	/* C	*/
	struct dsl_dataset	*rs_base;	/* A	*/
	struct dsl_dataset	*rs_right;	/* X	*/

	/* objsets for dnode/ZAP access */
	objset_t		*rs_left_os;
	objset_t		*rs_base_os;
	objset_t		*rs_right_os;

	/* root directory object numbers (from MASTER_NODE) */
	uint64_t		rs_left_root;
	uint64_t		rs_base_root;
	uint64_t		rs_right_root;

	/* collect phase output */
	rebase_changelist_t	rs_left_changes;
	rebase_changelist_t	rs_right_changes;

	/* cross-reference phase output */
	rebase_manifest_t	rs_manifest;
} rebase_state_t;

/*
 * MASTER_NODE ZAP keys for rebase-in-progress metadata.
 */
#define	ZFS_REBASE_MANIFEST	"org.openzfs:rebase_manifest"
#define	ZFS_REBASE_BASE_CLONE	"org.openzfs:rebase_base_clone"
#define	ZFS_REBASE_RIGHT_CLONE	"org.openzfs:rebase_right_clone"
#define	ZFS_REBASE_SNAP		"org.openzfs:rebase_snap"
#define	ZFS_REBASE_IN_PROGRESS	"org.openzfs:rebase_in_progress"

/*
 * Convenience name suffixes appended to the left dataset name.
 *
 * @%rebase-snap  — fence-post snapshot of the left HEAD taken before
 *                  any apply-phase mutations. Universal rollback
 *                  target: on error during apply, on --abort, and
 *                  for crash recovery.
 *
 * %rebase-base   — clone of the auto-discovered common ancestor (A),
 *                  used during phase 2 conflict resolution so the
 *                  user can diff base vs right.
 *
 * %rebase-right  — clone of the right snapshot, created only when
 *                  the right side was specified as a snapshot (has
 *                  '@'). Gives the user a mountable view for phase 2.
 *                  Not created when right is a HEAD dataset — the
 *                  user reads it directly.
 */
#define	ZFS_REBASE_SNAP_SUFFIX	"%rebase-snap"
#define	ZFS_REBASE_BASE_SUFFIX	"%rebase-base"
#define	ZFS_REBASE_RIGHT_SUFFIX	"%rebase-right"

int dsl_rebase(const char *left_ds, const char *right_ds,
    nvlist_t *outnvl);
int dsl_rebase_finish(const char *dsname);
int dsl_rebase_abort(const char *dsname);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_DSL_REBASE_H */
