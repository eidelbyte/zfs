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
 * The apply layer: object copy and stamping, data and xattr writes,
 * link, unlink, and rename primitives executed in the edit order the
 * emission stage produced.  The primitives are carried over from
 * revision 2 (branch zfs-rebase-two-axis) with review as the
 * apply-driver issue lands; the two-pass path-sorted driver of
 * revision 2 is not -- ordering belongs to the dependency solve.
 *
 * Populated by the emit epic's apply-driver issue; empty at the
 * module-stub stage.
 */

#include <sys/zfs_context.h>
#include <sys/dsl_rebase.h>
