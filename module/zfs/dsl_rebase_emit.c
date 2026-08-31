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
 * Emission: compiling the decision record into edits, the
 * dependency order with scratch-name rotation breaking
 * (Section 12), and the manifest -- the bounded summary nvlist and
 * the optional streamed form, sharing one serializer family.
 *
 * Populated by the emit and manifest epics; empty at the
 * module-stub stage.
 */

#include <sys/zfs_context.h>
#include <sys/dsl_rebase.h>
