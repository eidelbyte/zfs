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

#ifndef	_SYS_DSL_REBASE_H
#define	_SYS_DSL_REBASE_H

#include <sys/dmu.h>
#include <sys/zfs_context.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct dsl_dataset;

/*
 * ZFS rebase: replay the changes the left dataset (C) made since its
 * common ancestor (A) with the right snapshot (X), onto X -- a
 * three-way merge in the git-rebase shape.
 *
 * The diff engine models every path with two independent axes:
 *
 *   content axis  -- what happened to the data visible at the path
 *   linkpool axis -- what happened to the path's hardlink membership
 *
 * A "linkpool" is the set of paths all referencing the same dnode
 * through hardlinks. The axes are computed independently, so a
 * content no-op can never mask a membership change and vice versa.
 * Content ops are path-scoped: they never make identity claims about
 * the dnode under the path. Dnode identity through time ("lineage")
 * is tracked on the linkpool axis and in move detection only.
 */

/*
 * Every enum in this contract reserves 0 as its unset value (the
 * one exception is REBASE_POLICY_NONE, where "no policy" is itself
 * the zero meaning). A zeroed structure therefore never reads as a
 * real decision: assignments are always explicit, and consumers
 * VERIFY that a value they act on has left 0.
 */

/*
 * Content axis: what happened to the data visible at one path,
 * between the base snapshot and one side.
 */
typedef enum rebase_content_op {
	REBASE_CONTENT_UNSET = 0,	/* never a recorded value	*/
	REBASE_CONTENT_NONE,	/* no content change at this path	*/
	REBASE_CONTENT_ADD,	/* path absent in base, present in side	*/
	REBASE_CONTENT_DELETE,	/* path present in base, absent in side	*/
	/*
	 * Path in both, content differs. Covers both in-place rewrite
	 * and wholesale dnode replacement (rename-on-save); the
	 * replacement itself is the linkpool axis's business, not
	 * this one.
	 */
	REBASE_CONTENT_EDIT,
	REBASE_CONTENT_MOVE,	/* renamed/moved, content unchanged	*/
	REBASE_CONTENT_MOVE_EDIT /* renamed/moved and content changed	*/
} rebase_content_op_t;

/*
 * Linkpool axis: what happened to the path's linkpool membership,
 * between the base snapshot and one side.
 */
typedef enum rebase_linkpool_op {
	REBASE_LINKPOOL_UNSET = 0,	/* never a recorded value	*/
	REBASE_LINKPOOL_NONE,	/* membership unchanged vs base		*/
	REBASE_LINKPOOL_ADDED,	/* path joined a linkpool		*/
	REBASE_LINKPOOL_REMOVED, /* path left a linkpool (sever/delete)	*/
	REBASE_LINKPOOL_MOVED	/* left one linkpool, joined another	*/
} rebase_linkpool_op_t;

/*
 * A single two-axis change record; a path produces at most one
 * record per side. Appears in both AVL trees of its parent
 * changelist. Member prefix rc_ = "rebase change".
 */
typedef struct rebase_change {
	rebase_content_op_t	rc_content_op;	/* content axis		*/
	rebase_linkpool_op_t	rc_linkpool_op;	/* linkpool axis	*/
	uint64_t		rc_obj;		/* dnode object number	*/

	/*
	 * Linkpool provenance. rc_linkpool_from is the base linkpool
	 * this path left (REMOVED, MOVED); rc_linkpool_to is the side
	 * linkpool it joined (ADDED, MOVED); 0 when unused. These two
	 * fields make split and merge provenance mechanical in
	 * cross-reference phase A.
	 */
	uint64_t		rc_linkpool_from;
	uint64_t		rc_linkpool_to;

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
 *   by_path -- compound (path, obj) key; cross-referencing left
 *              vs right, and move-destination lookups
 *   by_obj  -- compound (obj, path) key; move-collapse scans where
 *              same-obj records sort adjacent
 * Member prefix rcl_ = "rebase changelist".
 */
typedef struct rebase_changelist {
	avl_tree_t	rcl_by_path;
	avl_tree_t	rcl_by_obj;
	uint_t		rcl_count;
} rebase_changelist_t;

struct rebase_linkpool;

/*
 * One membership entry of a linkpool: a single path referencing the
 * shared dnode. Lives on its owning linkpool's rlp_links list and in
 * the table-wide rlpt_by_path reverse index, so linkpool-of-path is
 * one AVL lookup, never a scan.
 * Member prefix rlpl_ = "rebase linkpool link".
 */
typedef struct rebase_linkpool_link {
	char			*rlpl_path;
	size_t			rlpl_pathlen;	/* for kmem_free	*/
	struct rebase_linkpool	*rlpl_owner;	/* back pointer		*/
	list_node_t		rlpl_node;	/* in rlp_links		*/
	avl_node_t		rlpl_avl;	/* in rlpt_by_path	*/
} rebase_linkpool_link_t;

/*
 * Anchor state: one mutually exclusive disposition per side
 * linkpool, decided in cross-reference phase A. An enum, not flag
 * bits: ANCHORED can never combine with NOVEL, RECYCLED is the
 * reason a linkpool is treated as novel rather than an independent
 * property, and SPLIT_FRAGMENT supersedes NOVEL. Illegal
 * combinations are unrepresentable. Zero is the walk-built default:
 * phase A must move every linkpool out of UNCLASSIFIED, and later
 * phases VERIFY that it did.
 */
typedef enum rebase_linkpool_state {
	REBASE_LINKPOOL_UNCLASSIFIED = 0, /* walk-built; phase A pending */
	REBASE_LINKPOOL_ANCHORED,	/* same lineage as a base object */
	REBASE_LINKPOOL_NOVEL,		/* born after the fork		*/
	REBASE_LINKPOOL_RECYCLED, /* obj number reused; treated as novel */
	/*
	 * Post-fork node severed from a base linkpool, identified by
	 * moved-from provenance; the parent lineage is in rlp_anchor.
	 */
	REBASE_LINKPOOL_SPLIT_FRAGMENT
} rebase_linkpool_state_t;

/*
 * A linkpool: the paths sharing one dnode via hardlinks, discovered
 * during the walk from ZPL_LINKS > 1. One per shared dnode per
 * branch (base, left, right). Member prefix rlp_ = "rebase linkpool".
 */
typedef struct rebase_linkpool {
	uint64_t	rlp_obj;	/* the shared dnode		*/
	uint64_t	rlp_nlink;	/* ZPL_LINKS at walk time	*/
	list_t		rlp_links;	/* rebase_linkpool_link_t	*/
	/* links seen by the walk; must equal rlp_nlink at walk end */
	uint_t		rlp_nfound;

	/* filled in by cross-reference phase A */
	rebase_linkpool_state_t	rlp_state;
	/* ANCHORED: own obj; SPLIT_FRAGMENT: parent lineage; else 0 */
	uint64_t	rlp_anchor;

	avl_node_t	rlp_avl;	/* in rlpt_by_obj		*/
} rebase_linkpool_t;

/*
 * Per-branch linkpool table; base, left, and right each get one.
 * Member prefix rlpt_ = "rebase linkpool table".
 */
typedef struct rebase_linkpool_table {
	avl_tree_t	rlpt_by_obj;	/* rebase_linkpool_t by rlp_obj	*/
	avl_tree_t	rlpt_by_path;	/* link reverse index (by path)	*/
	uint_t		rlpt_count;
} rebase_linkpool_table_t;

/*
 * Normalized membership target for one path on one side: the
 * branch-independent vocabulary that makes left and right directly
 * comparable in the per-path membership merge.
 */
typedef enum rebase_mtarget_kind {
	REBASE_TARGET_UNSET = 0,	/* never a built row's value	*/
	/*
	 * The side expressed nothing about this path. This is
	 * no-opinion, NOT a vote for the base state; it is what lets
	 * a lone sever or delete win without a conflict or a union.
	 */
	REBASE_TARGET_SAME_AS_BASE,
	REBASE_TARGET_GONE,		/* path deleted			*/
	REBASE_TARGET_STANDALONE,	/* path present, in no linkpool	*/
	REBASE_TARGET_ANCHOR,	/* member of an anchored linkpool	*/
	/*
	 * Member of a split fragment. Distinct from ANCHOR on
	 * purpose: an ANCHOR(parent) target here would let the
	 * membership merge re-join fragment members with the parent's
	 * roster, silently undoing the split.
	 */
	REBASE_TARGET_FRAGMENT,
	REBASE_TARGET_NOVEL	/* member of a post-fork linkpool	*/
} rebase_mtarget_kind_t;

/*
 * Membership target value.
 * Member prefix rmt_ = "rebase membership target".
 */
typedef struct rebase_mtarget {
	rebase_mtarget_kind_t	rmt_kind;
	/* ANCHOR: base lineage; FRAGMENT: parent lineage; NOVEL: id */
	uint64_t		rmt_linkpool;
	/* FRAGMENT only: unified fragment id (else 0) */
	uint64_t		rmt_fragment;
} rebase_mtarget_t;

/*
 * One row of the per-path membership merge (cross-reference phase D),
 * built for every path appearing in either changelist or either side
 * linkpool table. Untouched standalone paths never get a row.
 * Member prefix rpp_ = "rebase per-path".
 */
typedef struct rebase_ppath {
	char			*rpp_path;
	size_t			rpp_pathlen;	/* for kmem_free	*/
	rebase_mtarget_t	rpp_left;
	rebase_mtarget_t	rpp_right;
	rebase_mtarget_t	rpp_final;
	list_node_t		rpp_node;	/* in rlpg_members	*/
	avl_node_t		rpp_avl;	/* merge-wide row index	*/
} rebase_ppath_t;

/*
 * Which side's data a merged linkpool group carries.
 */
typedef enum rebase_content_src {
	REBASE_SRC_UNSET = 0,	/* never a decided group's value	*/
	REBASE_SRC_BASE,
	REBASE_SRC_LEFT,
	REBASE_SRC_RIGHT,
	REBASE_SRC_CONFLICT	/* unresolved; pre-merge data per rs_policy */
} rebase_content_src_t;

/*
 * A final linkpool group (membership-merge output, content-merge
 * input): at most one base lineage, the resolved member paths, and
 * one content decision for the whole group.
 * Member prefix rlpg_ = "rebase linkpool group".
 */
typedef struct rebase_linkpool_group {
	uint64_t		rlpg_lineage;	/* base obj, 0 if novel	*/
	uint64_t		rlpg_left_obj;	/* contributing objs	*/
	uint64_t		rlpg_right_obj;	/*   (0 if absent)	*/
	list_t			rlpg_members;	/* rebase_ppath_t rows	*/
	rebase_content_src_t	rlpg_src;
	avl_node_t		rlpg_avl;
} rebase_linkpool_group_t;

/*
 * Conflict types detected during cross-referencing. The first seven
 * are scoped to standalone (non-linkpool) paths; the last four are
 * produced by the linkpool merge.
 */
typedef enum rebase_conflict_type {
	REBASE_CONFLICT_UNSET = 0,	/* never a recorded value	*/
	REBASE_CONFLICT_BOTH_MODIFIED,	/* same path: EDIT on both sides */
	REBASE_CONFLICT_CREATE_CREATE,	/* same path: ADD on both sides	*/
	REBASE_CONFLICT_MODIFY_DELETE,	/* left EDIT, right DELETE	*/
	REBASE_CONFLICT_DELETE_MODIFY,	/* left DELETE, right EDIT	*/
	REBASE_CONFLICT_MOVE_DIVERGE,	/* same dnode MOVEd differently	*/
	REBASE_CONFLICT_MOVE_VS_EDIT,	/* one MOVE, other EDIT/DELETE	*/
	/* dir deleted on one side vs its contents edited on the other */
	REBASE_CONFLICT_DIR_DELETE_VS_EDIT,

	/* one side deleted the path, the other moved it into a linkpool */
	REBASE_CONFLICT_DELETE_VS_RELINK,
	/* both sides moved the path, into different linkpools */
	REBASE_CONFLICT_DIVERGENT_MEMBERSHIP,
	/*
	 * Per-linkpool three-way content conflict, surfaced once per
	 * group with the group's members as alt paths.
	 */
	REBASE_CONFLICT_LINKPOOL_CONTENT,
	/* novel linkpools claim the same paths with different data */
	REBASE_CONFLICT_NOVEL_LINKPOOL_OVERLAP
} rebase_conflict_type_t;

/*
 * A single conflict record. The manifest deduplicates conflicts on
 * the (rcf_obj, rcf_type) pair, never on obj alone: two conflicts of
 * different types on one hardlinked dnode are two records, and
 * rcf_alt_paths only ever merges same-type conflicts on the same
 * dnode. Member prefix rcf_ = "rebase conflict".
 */
typedef struct rebase_conflict {
	rebase_conflict_type_t	rcf_type;
	uint64_t		rcf_obj;
	char			*rcf_path;
	size_t			rcf_pathlen;

	/* other paths involved in the same (obj, type) conflict */
	char			**rcf_alt_paths;
	uint_t			rcf_nalt;

	list_node_t		rcf_node;	/* in rm_conflicts	*/
} rebase_conflict_t;

/*
 * Warnings: merge outcomes that are legal and resolved but worth
 * telling the user about. Emitted by the phase F consistency sweep;
 * never conflicts.
 */
typedef enum rebase_warning_kind {
	REBASE_WARN_UNSET = 0,		/* never a recorded value	*/
	/* linkpool resolution changed a path the losing side never saw */
	REBASE_WARN_IMPLIED_CHANGE,
	/* an edit won on a linkpool smaller than the editor believed */
	REBASE_WARN_LINKPOOL_SHRUNK,
	/* symlink target absent from the merged namespace */
	REBASE_WARN_DANGLING_SYMLINK
} rebase_warning_kind_t;

/*
 * A single warning record. Member prefix rw_ = "rebase warning".
 */
typedef struct rebase_warning {
	rebase_warning_kind_t	rw_kind;
	uint64_t		rw_obj;
	char			*rw_path;
	size_t			rw_pathlen;
	list_node_t		rw_node;	/* in rm_warnings	*/
} rebase_warning_t;

/*
 * Apply actions. The cross-reference phase compiles its decisions
 * into these; the apply phase consumes actions, never raw
 * changelists.
 */
typedef enum rebase_action_type {
	REBASE_ACTION_UNSET = 0, /* never a recorded value		*/
	REBASE_ACTION_LINK,	/* new dir entry to a linkpool node	*/
	REBASE_ACTION_UNLINK,	/* drop dir entry; node survives	*/
	REBASE_ACTION_WRITE,	/* materialize group content from src	*/
	/*
	 * Copy a node out to standalone, content frozen at the
	 * severing side's snapshot.
	 */
	REBASE_ACTION_SEVER,
	REBASE_ACTION_COPY	/* plain non-linkpool copy		*/
} rebase_action_type_t;

/*
 * A single apply action. Member prefix ra_ = "rebase action".
 */
typedef struct rebase_action {
	rebase_action_type_t	ra_type;
	char			*ra_path;	/* dir entry acted on	*/
	size_t			ra_pathlen;	/* for kmem_free	*/
	/* dnode in the left HEAD the action targets (0 = created) */
	uint64_t		ra_obj;
	/* which objset the content or link source comes from */
	rebase_content_src_t	ra_src;
	uint64_t		ra_src_obj;	/* dnode there (0 = n/a) */
	list_node_t		ra_node;	/* in rm_actions	*/
} rebase_action_t;

/*
 * The manifest: the output of the cross-reference phase and the
 * input to the emit and apply phases.
 * Member prefix rm_ = "rebase manifest".
 */
typedef struct rebase_manifest {
	list_t		rm_conflicts;	/* rebase_conflict_t		*/
	uint_t		rm_nconflicts;
	list_t		rm_warnings;	/* rebase_warning_t		*/
	uint_t		rm_nwarnings;
	list_t		rm_actions;	/* rebase_action_t		*/
	uint_t		rm_nactions;
} rebase_manifest_t;

/*
 * Tie-break policy for near-equivalent or unresolvable outcomes,
 * from the CLI's --left/--right/--base/--neither flags.
 */
typedef enum rebase_policy {
	/*
	 * No preference given; policy-settled ties surface as
	 * conflicts instead. Deliberately the zero value: "no
	 * policy" is exactly what an unset field means.
	 */
	REBASE_POLICY_NONE = 0,
	REBASE_POLICY_LEFT,
	REBASE_POLICY_RIGHT,
	REBASE_POLICY_BASE,
	REBASE_POLICY_NEITHER
} rebase_policy_t;

/*
 * Top-level rebase operation state. Allocated at the start of
 * dsl_rebase() and torn down on completion or error.
 * Member prefix rs_ = "rebase state".
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

	/*
	 * The fork point: ds_creation_txg of the auto-discovered
	 * common ancestor. Left and right are clones in one zpool,
	 * so txgs come from one SPA counter, and a dnode whose newest
	 * birth txg is <= rs_fork_txg is untouched since the fork --
	 * an integer compare, no data reads. txg comparisons are only
	 * ever meaningful against rs_fork_txg; never compare left
	 * txgs to right txgs, because both sides allocate from the
	 * shared counter concurrently after the fork.
	 */
	uint64_t		rs_fork_txg;

	/* walk phase output */
	rebase_linkpool_table_t	rs_base_linkpools;
	rebase_linkpool_table_t	rs_left_linkpools;
	rebase_linkpool_table_t	rs_right_linkpools;

	/* collect phase output */
	rebase_changelist_t	rs_left_changes;
	rebase_changelist_t	rs_right_changes;

	/* cross-reference phase output */
	rebase_manifest_t	rs_manifest;

	rebase_policy_t		rs_policy;	/* conflict tie-breaks	*/
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
 * @%rebase-snap -- fence-post snapshot of the left HEAD, taken
 *                  immediately after preconditions pass (creation
 *                  commits the ZIL; the diff engine never walks a
 *                  live objset). Universal rollback target: on error
 *                  during apply, on --abort, and for crash recovery.
 *
 * @%rebase-right-snap -- fence-post snapshot of the right side,
 *                  created only when right was given as a live HEAD
 *                  dataset; the diff engine reads right through it.
 *                  Not created when right is already a snapshot.
 *
 * %rebase-base  -- clone of the auto-discovered common ancestor (A),
 *                  used during phase 2 conflict resolution so the
 *                  user can diff base vs right.
 *
 * %rebase-right -- clone of the right snapshot, created only when
 *                  the right side was specified as a snapshot (has
 *                  '@'). Gives the user a mountable view for phase 2.
 *                  Not created when right is a HEAD dataset -- the
 *                  user reads it directly.
 */
#define	ZFS_REBASE_SNAP_SUFFIX	"%rebase-snap"
#define	ZFS_REBASE_RIGHT_SNAP_SUFFIX	"%rebase-right-snap"
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
