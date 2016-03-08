/*
 * Copyright (c) 2015 Cray Inc. All rights reserved.
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Endpoint common code
 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "gnix_freelist.h"
#include "gnix_util.h"

/*
 * NOTES:
 * - thread safe if initialized with _nix_sfl_init_ts
 * - Does not shrink
 * - Cannot be used for data structures with alignment requirements
 * - Refill size increases by growth_factor each time growth is needed
 *   (limited by max_refill_size)
 * - Refills are allocated as chunks, which are managed by chunks slist
 * - Allocate an extra element at the beginning of each chunk for the
 *   chunk slist
 * - Individual elements are *not* zeroed before being returned
 *
 * Your structure doesn't really need to have an slist_entry pointer,
 * it just has to be at least as big as an slist_entry.
 */

static int __gnix_sfl_refill(struct gnix_s_freelist *fl, int n)
{	int i, ret = FI_SUCCESS;
	unsigned char *elems;

	assert(fl);
	assert(n > 0);
	/*
	 * We allocate an extra element for use as the pointer to the
	 * memory chunk maintained in the chunks field for later
	 * freeing.  Use an entire element, in case size was padded
	 * for alignment
	 */
	elems = calloc((n+1), fl->elem_size);
	if (elems == NULL) {
		ret = -FI_ENOMEM;
		goto err;
	}

	/* Save away the pointer to the chunk */
	slist_insert_tail((struct slist_entry *) elems, &fl->chunks);

	/* Start with slist_entry of first element */
	elems += fl->elem_size + fl->offset;

	for (i = 0; i < n; i++) {
		dlist_init((struct dlist_entry *) elems);
		dlist_insert_tail((struct dlist_entry *) elems, &fl->freelist);
		elems += fl->elem_size;
	}
err:
	return ret;
}

int _gnix_sfl_init(int elem_size, int offset, int init_size,
		   int refill_size, int growth_factor,
		   int max_refill_size, struct gnix_s_freelist *fl)
{
	assert(elem_size > 0);
	assert(offset >= 0);
	assert(init_size >= 0);
	assert(refill_size >= 0);
	assert(growth_factor >= 0);
	assert(max_refill_size >= 0);

	int fill_size = init_size != 0 ? init_size : GNIX_SFL_INIT_SIZE;

	fl->refill_size = (refill_size != 0 ?
			   refill_size :
			   GNIX_SFL_INIT_REFILL_SIZE);
	fl->growth_factor = (growth_factor != 0 ?
			     growth_factor :
			     GNIX_SFL_GROWTH_FACTOR);
	fl->max_refill_size = (max_refill_size != 0 ?
			       max_refill_size :
			       fill_size);
	fl->elem_size = elem_size;
	fl->offset = offset;

	dlist_init(&fl->freelist);
	assert(slist_empty(&fl->chunks)); /* maybe should be a warning? */
	slist_init(&fl->chunks);

	return __gnix_sfl_refill(fl, fill_size);
}

int _gnix_sfl_init_ts(int elem_size, int offset, int init_size,
		      int refill_size, int growth_factor,
		      int max_refill_size, struct gnix_s_freelist *fl)
{
	int ret;

	ret = _gnix_sfl_init(elem_size,
			     offset,
			     init_size,
			     refill_size,
			     growth_factor,
			     max_refill_size,
			     fl);
	if (ret == FI_SUCCESS) {
		fl->ts = 1;
		fastlock_init(&fl->lock);
	}

	return ret;

}

void _gnix_sfl_destroy(struct gnix_s_freelist *fl)
{
	assert(fl);

	struct slist_entry *chunk;

	for (chunk = slist_remove_head(&fl->chunks);
	     chunk != NULL;
	     chunk = slist_remove_head(&fl->chunks)) {
		free(chunk);
	}

	if (fl->ts)
		fastlock_destroy(&fl->lock);
}

int _gnix_sfe_alloc(struct dlist_entry **e, struct gnix_s_freelist *fl)
{
	int ret = FI_SUCCESS;
	struct dlist_entry *de;

	assert(fl);

	if (fl->ts)
		fastlock_acquire(&fl->lock);

	if (dlist_empty(&fl->freelist)) {
		ret = __gnix_sfl_refill(fl, fl->refill_size);
		if (ret != FI_SUCCESS)
			goto err;
		if (fl->refill_size < fl->max_refill_size) {
			int ns = fl->refill_size *= fl->growth_factor;

			fl->refill_size = (ns >= fl->max_refill_size ?
					   fl->max_refill_size :
					   ns);
		}

		if (dlist_empty(&fl->freelist)) {
			/* Can't happen unless multithreaded */
			ret = -FI_EAGAIN;
			goto err;
		}
	}

	de = fl->freelist.next;
	dlist_remove_init(de);

	*e = de;
err:
	if (fl->ts)
		fastlock_release(&fl->lock);
	return ret;
}

void _gnix_sfe_free(struct dlist_entry *e, struct gnix_s_freelist *fl)
{
	assert(e);
	assert(fl);

	e->next = NULL;  /* keep slist implementation happy */

	if (fl->ts)
		fastlock_acquire(&fl->lock);
	dlist_init(e);
	dlist_insert_tail(e, &fl->freelist);
	if (fl->ts)
		fastlock_release(&fl->lock);
}

