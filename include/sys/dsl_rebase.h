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

#ifndef	_SYS_DSL_REBASE_H
#define	_SYS_DSL_REBASE_H

#include <sys/dsl_dataset.h>
#include <sys/dsl_pool.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Find the most recent common ancestor snapshot of two datasets.
 *
 * Both datasets must be on the same pool. Walks the snapshot chain
 * (ds_prev_snap_obj) and clone origin (dd_origin_obj) of each dataset
 * to find the most recent snapshot present in both chains.
 *
 * On success, *ancestor is held (caller must dsl_dataset_rele).
 * Returns ENOENT if no common ancestor exists.
 */
int dsl_rebase_find_ancestor(dsl_pool_t *dp, dsl_dataset_t *base,
    dsl_dataset_t *after, const void *tag, dsl_dataset_t **ancestor);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_DSL_REBASE_H */
