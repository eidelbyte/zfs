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
 * The decide engine: faces and pairing, components, succession and
 * the one-move rule, pass 0 (lineage), pass 1 (names), pass 2
 * (pooling), pass 3 (content), pass 4 (ancestry), and the
 * quarantine fixed point.  Consumes the model dsl_rebase.c's walk
 * built; produces the decision record of sys/dsl_rebase.h.
 *
 * Populated by the decide epic, one pass per commit; empty at the
 * module-stub stage.
 */

#include <sys/zfs_context.h>
#include <sys/dsl_rebase.h>
