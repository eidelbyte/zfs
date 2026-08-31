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
#include <sys/sa.h>
#include <sys/zfs_context.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct dsl_dataset;
struct dsl_pool;

/*
 * ZFS rebase, revision 3: replay the changes the left dataset made
 * since its common ancestor with the right snapshot, onto the right
 * snapshot -- a three-way merge in the git-rebase shape, decided by
 * the phase-3 graph theory.
 *
 * The model.  Each of the three trees (base = the common ancestor,
 * onto = the right side, off-of = the left side) is a set of POOLS.
 * A pool is one dnode together with every name (full path) that
 * reaches it; a one-name pool is the ordinary case, and every dnode
 * the walk reaches is a pool.  Pool identity is the pair (dnode
 * index, birth txg), guarded by ZPL_GEN against recycling.  Evidence
 * between trees lives on three FACES (base-onto, base-offof, and the
 * onto-offof cut): green edges record shared names, red edges record
 * dnode identity (raw on the base faces, composed through base on
 * the cut -- raw identity across the cut is false identity, because
 * clone siblings allocate in lockstep), and the yellow relation
 * records content equality.  Pools paired across a face by red and
 * green nomination decide together; connected components of green
 * and base-face red are the decision unit.
 *
 * Polarity.  Onto is the RIGHT side: the output tree begins as a
 * clone of it, and a quiet off-of decides to onto's state verbatim.
 * Off-of is the LEFT side, whose changes are replayed.  No record or
 * report ever says left or right; the vocabulary is onto and off-of
 * throughout, so a component reads identically whichever way the
 * rebase runs.
 *
 * The passes.  Pass 0 (lineage) classifies each base pool's fate on
 * each side and conflicts the crossings Rule 7.2 forbids.  The
 * one-move rule then conflicts any base name moved by both faces
 * without wholesale agreement.  Pass 1 (names) decides survival per
 * name by the five-row cell table.  Pass 2 (pooling) partitions the
 * survivors into output pools through succession, links, and
 * exclusions, with a link chain as the infeasibility certificate.
 * Pass 3 (content) chooses each output pool's bytes by lookup-name
 * contributors and the guarded three-way, with a duplication guard.
 * Pass 4 (ancestry) checks the survivors' structure.  A quarantine
 * fixed point holds back every component a conflict touches, and
 * emission compiles the rest into a dependency-ordered edit list
 * against the clone.
 *
 * Every enum in this contract reserves 0 as its unset value, so a
 * zeroed structure never reads as a real decision.  Enums that are
 * array INDICES rather than decisions (face and side indices below)
 * are exempt and say so where they are defined.
 */

/*
 * ------------------------------------------------------------------
 * Identifiers
 * ------------------------------------------------------------------
 *
 * Names are interned once into a global sorted table over the union
 * of the three trees' names; a name identifier is the name's rank in
 * lexicographic order.  Pools are referenced by their index into
 * their own tree's pool array, which is sorted by (index, txg).
 * Path text and formatted pool keys exist only at the manifest
 * boundary.
 */

typedef uint32_t rebase_name_id_t;
typedef uint32_t rebase_pool_idx_t;

#define	REBASE_NO_NAME	((rebase_name_id_t)-1)
#define	REBASE_NO_POOL	((rebase_pool_idx_t)-1)

typedef enum rebase_tree_tag {
	REBASE_TREE_UNSET = 0,
	REBASE_TREE_BASE,
	REBASE_TREE_ONTO,
	REBASE_TREE_OFFOF
} rebase_tree_tag_t;

#define	REBASE_TREES		3
/* Dense array index for a set tree tag. */
#define	REBASE_TIDX(tag)	((int)(tag) - 1)

/*
 * Face and side indices.  These are array indices, not decisions,
 * and are exempt from the zero-unset rule.  The two SIDE indices
 * name the base faces, which is where succession and the survivor
 * records live; the cut has no succession.
 */
typedef enum rebase_face_idx {
	REBASE_FACE_BASE_ONTO = 0,
	REBASE_FACE_BASE_OFFOF,
	REBASE_FACE_CUT,
	REBASE_FACES
} rebase_face_idx_t;

#define	REBASE_SIDE_ONTO	0
#define	REBASE_SIDE_OFFOF	1
#define	REBASE_SIDES		2

typedef enum rebase_node_type {
	REBASE_NTYPE_UNSET = 0,
	REBASE_NTYPE_FILE,	/* any non-directory; detail in rp_mode */
	REBASE_NTYPE_DIR
} rebase_node_type_t;

/*
 * ------------------------------------------------------------------
 * The name table
 * ------------------------------------------------------------------
 *
 * Built during the walk and finalized once: sorted paths, parent
 * links, children in CSR form, and one holder map per tree.  The
 * holder maps replace every by-name lookup in the engine; membership
 * of name n in pool q is the single read holder[tree][n] == q.
 */
typedef struct rebase_name_table {
	char		**rnt_names;		/* sorted full paths */
	uint32_t	rnt_count;
	rebase_name_id_t *rnt_parent;		/* REBASE_NO_NAME at "/" */
	uint32_t	*rnt_child_start;	/* rnt_count + 1 entries */
	rebase_name_id_t *rnt_child_list;
	rebase_pool_idx_t *rnt_holder[REBASE_TREES];
} rebase_name_table_t;

/*
 * ------------------------------------------------------------------
 * Content (the yellow relation)
 * ------------------------------------------------------------------
 *
 * Content equality is resolved in two tiers: pools untouched since
 * the fork txg inherit their base counterpart's class for free, and
 * the remaining pairs the passes actually ask about run the pairwise
 * tier stack (generation guard, SA identity, logical xattrs, block
 * pointer proofs including the spill pointer, bounded byte compare)
 * behind a memoized cache.  rc_class zero means unresolved.
 */
typedef struct rebase_content {
	uint32_t	rc_class;
	uint64_t	rc_size;
	uint64_t	rc_sa_hash;
	boolean_t	rc_untouched;	/* logical birth <= fork txg */
} rebase_content_t;

/*
 * ------------------------------------------------------------------
 * Pools and trees
 * ------------------------------------------------------------------
 */
typedef struct rebase_pool_id {
	rebase_tree_tag_t rpi_tree;
	uint64_t	rpi_index;	/* dnode object number */
	uint64_t	rpi_txg;	/* birth txg (identity txg) */
} rebase_pool_id_t;

typedef struct rebase_pool {
	rebase_pool_id_t rp_id;
	rebase_node_type_t rp_type;
	uint64_t	rp_mode;	/* ZPL_MODE */
	uint64_t	rp_gen;		/* ZPL_GEN, recycling guard */
	uint32_t	rp_nnames;
	rebase_name_id_t *rp_names;	/* sorted */
	uint32_t	rp_component;
	rebase_content_t rp_content;
	/* order-independent, for the equal-name-set fast test */
	uint64_t	rp_nameset_hash;
} rebase_pool_t;

typedef struct rebase_tree {
	rebase_tree_tag_t rt_tag;
	objset_t	*rt_os;		/* borrowed from the hold */
	sa_attr_type_t	*rt_sa_table;	/* per objset, never shared */
	uint64_t	rt_root_obj;
	rebase_pool_t	*rt_pools;	/* sorted by (index, txg) */
	uint32_t	rt_npools;
} rebase_tree_t;

/*
 * ------------------------------------------------------------------
 * Faces
 * ------------------------------------------------------------------
 *
 * Green edges are built by iterating names, never pool pairs.  Share
 * labels are not stored; the decision layer needs only endpoints,
 * the share count, and fullness at each end.  Red is a partial
 * matching; on the cut it holds derived red only.
 */
typedef struct rebase_green_edge {
	rebase_pool_idx_t rge_a;
	rebase_pool_idx_t rge_b;
	uint32_t	rge_share;
	boolean_t	rge_full_a;
	boolean_t	rge_full_b;
} rebase_green_edge_t;

typedef struct rebase_face {
	rebase_tree_tag_t rf_a;
	rebase_tree_tag_t rf_b;
	rebase_green_edge_t *rf_green;
	uint32_t	rf_ngreen;
	/* per pool: green degree, and the edge when degree is one */
	uint32_t	*rf_gdeg_a;
	uint32_t	*rf_gdeg_b;
	uint32_t	*rf_gedge_a;
	uint32_t	*rf_gedge_b;
	/* red matching, both directions */
	rebase_pool_idx_t *rf_red_ab;
	rebase_pool_idx_t *rf_red_ba;
	/* pairing result (Definition 6.1) */
	rebase_pool_idx_t *rf_paired_ab;
	rebase_pool_idx_t *rf_paired_ba;
	uint8_t		*rf_cand_n_a;	/* candidate counts: 0, 1, 2 */
	uint8_t		*rf_cand_n_b;
} rebase_face_t;

/*
 * ------------------------------------------------------------------
 * Components
 * ------------------------------------------------------------------
 *
 * Members and names live in shared CSR arrays sorted by component;
 * identifiers are assigned by sorting components on (first name,
 * first member identity), and conflict and quarantine records carry
 * them, so the ordering is contract.  Survivor and death slices are
 * per component (Definition 11.1).
 */
typedef struct rebase_component {
	uint32_t	rcm_id;
	uint32_t	rcm_first_pool;	/* into the member CSR */
	uint32_t	rcm_npools;
	uint32_t	rcm_first_name;	/* into the name CSR */
	uint32_t	rcm_nnames;
	uint32_t	rcm_first_survivor;
	uint32_t	rcm_nsurvivors;
	uint32_t	rcm_first_death;
	uint32_t	rcm_ndeaths;
} rebase_component_t;

/*
 * ------------------------------------------------------------------
 * Succession, substitution, and the one-move record
 * ------------------------------------------------------------------
 *
 * Per side face, indexed by name id.  The moved record feeds the
 * one-move rule of Definition 9.3: a lost name absent from the
 * side's entire tree, in a paired pool that gained at least one
 * name, was moved on that face, and the face's gained set is
 * recorded whether or not a succession was assigned.  Two moves of
 * one name reconcile only when the two side pools carry identical
 * name sets; anything else is a name conflict, and the certificate
 * never resolves an ambiguous gain set to a single target.
 */
typedef struct rebase_succession {
	rebase_name_id_t *rsc_antecedent[REBASE_SIDES];
	rebase_name_id_t *rsc_successor[REBASE_SIDES];
	uint8_t		*rsc_attachment[REBASE_SIDES];	/* bitmaps */
	uint8_t		*rsc_moved[REBASE_SIDES];
	rebase_name_id_t **rsc_moved_gains[REBASE_SIDES];
	uint8_t		*rsc_moved_ngains[REBASE_SIDES];
} rebase_succession_t;

/*
 * The precomputed survivor record: every predicate pass 2 and pass 3
 * repeat, collapsed into fields before either runs.
 */
typedef struct rebase_survivor {
	rebase_name_id_t rsv_name;
	rebase_name_id_t rsv_sub;	/* substituted name */
	rebase_pool_idx_t rsv_base_holder;
	rebase_pool_idx_t rsv_hstar[REBASE_SIDES];	/* holder-star */
	rebase_pool_idx_t rsv_source[REBASE_SIDES];	/* Def 9.4 */
	uint8_t		rsv_attach[REBASE_SIDES];
} rebase_survivor_t;

/*
 * ------------------------------------------------------------------
 * Fates and cells
 * ------------------------------------------------------------------
 */
typedef enum rebase_fate {
	REBASE_FATE_UNSET = 0,
	REBASE_FATE_CONTINUED_UNCHANGED,
	REBASE_FATE_CONTINUED_CHANGED,
	REBASE_FATE_DEAD,
	REBASE_FATE_CONTESTED,
	REBASE_FATE_DISTURBED
} rebase_fate_t;

typedef enum rebase_cell_outcome {
	REBASE_CELL_UNSET = 0,
	REBASE_CELL_KEEP,
	REBASE_CELL_ADOPT,
	REBASE_CELL_CONFLICT	/* a death for survivorship purposes */
} rebase_cell_outcome_t;

/*
 * ------------------------------------------------------------------
 * Output pools
 * ------------------------------------------------------------------
 */
typedef enum rebase_realization {
	REBASE_REAL_UNSET = 0,
	REBASE_REAL_ONTO_DNODE,
	REBASE_REAL_MATERIALIZED
} rebase_realization_t;

typedef struct rebase_outpool {
	uint32_t	rop_id;		/* dense, name-ordered */
	uint32_t	rop_component;
	rebase_name_id_t *rop_names;	/* sorted */
	uint32_t	rop_nnames;
	rebase_node_type_t rop_type;
	/* content choice: take the bytes of this pool of this tree */
	rebase_tree_tag_t rop_taken_tree;
	rebase_pool_idx_t rop_taken_from;
	uint8_t		rop_rule;	/* 1, 2, or 3 */
	rebase_realization_t rop_realization;
	/* the onto pool reused; REBASE_NO_POOL when materialized */
	rebase_pool_idx_t rop_clone_dnode;
} rebase_outpool_t;

/*
 * ------------------------------------------------------------------
 * Conflicts and certificates
 * ------------------------------------------------------------------
 *
 * The kernel emits a message code and a machine-readable
 * certificate; userland renders the sentence.  Conflicts sort on
 * (kind, primary component, certificate anchor) -- never on message
 * text.  No certificate field or rendered message may use left or
 * right vocabulary.
 */
typedef enum rebase_conflict_kind {
	REBASE_CONFLICT_UNSET = 0,
	REBASE_CONFLICT_LINEAGE,
	REBASE_CONFLICT_NAME,
	REBASE_CONFLICT_POOLING,
	REBASE_CONFLICT_CONTENT,
	REBASE_CONFLICT_STRUCTURAL
} rebase_conflict_kind_t;

typedef enum rebase_conflict_msg {
	REBASE_MSG_UNSET = 0,
	REBASE_MSG_LINEAGE_DISAGREE,
	REBASE_MSG_NAME_REHOMED,
	REBASE_MSG_NAME_MOVED_TWICE,
	REBASE_MSG_POOLING_EXCLUDED_JOINED,
	REBASE_MSG_CONTENT_NO_CHOICE,
	REBASE_MSG_CONTENT_DUPLICATION,
	REBASE_MSG_STRUCT_PARENT_GONE,
	REBASE_MSG_STRUCT_PARENT_NOT_DIR,
	REBASE_MSG_STRUCT_MULTI_NAME_DIR
} rebase_conflict_msg_t;

typedef struct rebase_cert_lineage {
	rebase_pool_idx_t rcl_pool;	/* base pool */
	rebase_fate_t	rcl_fate_onto;
	rebase_fate_t	rcl_fate_offof;
} rebase_cert_lineage_t;

/*
 * Two shapes share the name kind.  The re-homed variant (pass 1,
 * row 5) carries the two side holders; the one-move variant carries
 * each face's successor or whole gained set, sorted, never resolving
 * an ambiguous set to one name.
 */
typedef struct rebase_cert_name {
	rebase_name_id_t rcn_name;
	uint8_t		rcn_variant;	/* 1 re-homed, 2 one-move */
	rebase_pool_idx_t rcn_holder_onto;
	rebase_pool_idx_t rcn_holder_offof;
	rebase_name_id_t *rcn_moved[REBASE_SIDES];
	uint8_t		rcn_nmoved[REBASE_SIDES];
} rebase_cert_name_t;

typedef struct rebase_cert_pooling {
	rebase_name_id_t rcp_excluded[2];
	boolean_t	rcp_severed_onto;
	boolean_t	rcp_severed_offof;
	rebase_name_id_t *rcp_chain;	/* breadth-first link path */
	uint32_t	rcp_nchain;
} rebase_cert_pooling_t;

typedef struct rebase_cert_content {
	uint32_t	*rcc_outpools;
	uint32_t	rcc_noutpools;
	uint8_t		rcc_rule;
	/* contributor pools per tree: the certificate's own copy */
	rebase_pool_idx_t *rcc_contrib[REBASE_TREES];
	uint32_t	rcc_ncontrib[REBASE_TREES];
	/* duplication-guard variant: the edited side pool */
	rebase_tree_tag_t rcc_contributor_tree;
	rebase_pool_idx_t rcc_contributor;
} rebase_cert_content_t;

typedef struct rebase_cert_structural {
	rebase_name_id_t rcs_survivor;	/* parent rules only */
	rebase_name_id_t rcs_parent;	/* parent rules only */
	rebase_name_id_t *rcs_names;	/* multi-name-dir only */
	uint32_t	rcs_nnames;
} rebase_cert_structural_t;

typedef struct rebase_conflict {
	rebase_conflict_kind_t rcf_kind;
	rebase_conflict_msg_t rcf_msg;
	uint32_t	rcf_component;	/* primary */
	uint32_t	*rcf_components; /* all implicated, sorted */
	uint32_t	rcf_ncomponents;
	union {
		rebase_cert_lineage_t	rcfu_lineage;
		rebase_cert_name_t	rcfu_name;
		rebase_cert_pooling_t	rcfu_pooling;
		rebase_cert_content_t	rcfu_content;
		rebase_cert_structural_t rcfu_structural;
	} rcf_cert;
} rebase_conflict_t;

/*
 * ------------------------------------------------------------------
 * Quarantine
 * ------------------------------------------------------------------
 */
typedef enum rebase_qreason {
	REBASE_QREASON_UNSET = 0,
	REBASE_QREASON_CONFLICT,
	REBASE_QREASON_PARENT,
	REBASE_QREASON_CHILD
} rebase_qreason_t;

#define	REBASE_NO_COMPONENT	((uint32_t)-1)

typedef struct rebase_quarantine_entry {
	uint32_t	rq_component;
	rebase_qreason_t rq_reason;
	/* REBASE_NO_COMPONENT when the reason is a conflict */
	uint32_t	rq_dragged_by;
} rebase_quarantine_entry_t;

/*
 * ------------------------------------------------------------------
 * Edits (Section 12)
 * ------------------------------------------------------------------
 *
 * A rename is the atomic move: a creator of the new name and a
 * remover of the old for dependency purposes.  Scratch names break
 * rotation cycles, are allocated by the engine, and cross the
 * manifest marked as machinery.  The edit list is global and
 * interleaves components; re_order is the topological position.
 */
typedef enum rebase_edit_verb {
	REBASE_EDIT_UNSET = 0,
	REBASE_EDIT_UNLINK,
	REBASE_EDIT_LINK,
	REBASE_EDIT_RENAME,
	REBASE_EDIT_WRITE,
	REBASE_EDIT_MATERIALIZE
} rebase_edit_verb_t;

typedef struct rebase_edit {
	rebase_edit_verb_t re_verb;
	rebase_name_id_t re_name;
	rebase_name_id_t re_name2;	/* rename target or scratch */
	uint32_t	re_outpool;
	uint32_t	re_order;
} rebase_edit_t;

/*
 * ------------------------------------------------------------------
 * The decision record (Definition 11.1)
 * ------------------------------------------------------------------
 *
 * The complete in-memory result of a decide run, and the test
 * interface: the harness asserts against this structure, never
 * against scraped debug text.  Diagnostics, when built, hang off
 * rd_diag and alter nothing here.
 */
struct rebase_diag;

typedef struct rebase_decision {
	rebase_name_table_t *rd_names;
	rebase_tree_t	*rd_trees;	/* the run's three trees */
	rebase_component_t *rd_components;
	uint32_t	rd_ncomponents;
	rebase_pool_idx_t *rd_member_csr; /* component member lists */
	rebase_name_id_t *rd_name_csr;	/* component name lists */
	rebase_name_id_t *rd_survivor_csr;
	rebase_name_id_t *rd_death_csr;
	uint8_t		*rd_survives;	/* bitmap by name id */
	rebase_cell_outcome_t *rd_outcome; /* by name id */
	rebase_outpool_t *rd_outpools;
	uint32_t	rd_noutpools;
	rebase_conflict_t *rd_conflicts; /* sorted, see above */
	uint32_t	rd_nconflicts;
	rebase_quarantine_entry_t *rd_quarantine;
	uint32_t	rd_nquarantine;
	rebase_edit_t	*rd_edits;	/* in application order */
	uint32_t	rd_nedits;
	struct rebase_diag *rd_diag;	/* NULL unless diagnostics on */
} rebase_decision_t;

/*
 * ------------------------------------------------------------------
 * Run state and memory
 * ------------------------------------------------------------------
 *
 * One decide run allocates from one arena so that teardown is one
 * release and two runs can coexist (the polarity mirror check runs
 * the engine twice and compares records).  The memory cap is
 * enforced at the arena allocator, the single enforcement point;
 * hitting it fails the operation cleanly before any mutation.
 */
typedef struct rebase_arena {
	list_t		ra_blocks;
	uint64_t	ra_used;
	uint64_t	ra_limit;
} rebase_arena_t;

typedef struct rebase_run {
	/* transient; not held across the long-running work */
	struct dsl_pool	*rr_dp;
	/*
	 * The three read sources, indexed by REBASE_TIDX: base and
	 * the fence snapshots (or the right snapshot as given).
	 * The left HEAD is the apply target, never a read source,
	 * and is held separately; the named right head is held when
	 * right arrived as a live head (NULL when it arrived as a
	 * snapshot, in which case rr_ds[ONTO] is that snapshot).
	 */
	struct dsl_dataset *rr_ds[REBASE_TREES];
	struct dsl_dataset *rr_left_head;
	struct dsl_dataset *rr_right_head;
	boolean_t	rr_left_fence_created;
	boolean_t	rr_right_fence_created;
	uint64_t	rr_fork_txg;
	rebase_arena_t	rr_arena;
	rebase_name_table_t rr_names;
	rebase_tree_t	rr_trees[REBASE_TREES];
	rebase_face_t	rr_faces[REBASE_FACES];
	rebase_succession_t rr_succ;
	rebase_survivor_t *rr_survivors;
	uint32_t	rr_nsurvivors;
	rebase_decision_t rr_decision;
} rebase_run_t;

/*
 * Memory cap for one decide run's arena, in bytes; 0 means the
 * built-in default.  Checked at allocation time only.
 */
extern uint64_t rebase_mem_limit_bytes;

/*
 * Fence-post snapshots.  The engine never walks a live objset: the
 * left head is fenced immediately after preconditions pass, and a
 * right given as a live head gets its own fence.  '%' names are
 * kernel-internal and invisible to generic commands.
 */
#define	ZFS_REBASE_SNAP_SUFFIX		"%rebase-snap"
#define	ZFS_REBASE_RIGHT_SNAP_SUFFIX	"%rebase-right-snap"

int dsl_rebase(const char *left_ds, const char *right_ds,
    nvlist_t *outnvl);
int dsl_rebase_finish(const char *dsname);
int dsl_rebase_abort(const char *dsname);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_DSL_REBASE_H */
