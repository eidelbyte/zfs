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

int
dsl_rebase(const char *left_ds, const char *right_ds, nvlist_t *outnvl)
{
	(void) left_ds;
	(void) right_ds;
	(void) outnvl;

	return (SET_ERROR(ENOSYS));
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
