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
#include <sys/dmu.h>
#include <sys/spa.h>
#include <sys/nvpair.h>

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
