/*
 *  Copyright (C) 2020 - Vito Caputo - <vcaputo@pengaru.com>
 *
 *  This program is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 3 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <liburing.h>
#include <stdlib.h>
#include <stddef.h>

#include "iou.h"

/* iou is a minimal IO-oriented async callback scheduler built atop io_uring */

typedef struct _iou_op_t _iou_op_t;
typedef struct iou_ops_t iou_ops_t;

typedef struct iou_t {
	struct io_uring	ring;
	unsigned	n_issued, n_queued, n_submitted;
	unsigned	quit:1;
	iou_ops_t	*ops;
} iou_t;

/* private container of the public iou_op_t */
struct _iou_op_t {
	iou_op_t	public;

	int		(*cb)(void *cb_data);
	void		*cb_data;
	_iou_op_t	*free_next;
	iou_ops_t	*container;
};

/* ops bulk allocation container */
struct iou_ops_t {
	iou_ops_t	*next;
	_iou_op_t	*free;
	size_t		count;
	_iou_op_t	ops[];
};

#ifndef CONTAINER_OF
#define CONTAINER_OF(ptr, type, member) \
(type *)((char *)(ptr) - offsetof(type, member))
#endif


static _iou_op_t * ops_get(iou_ops_t **ops)
{
	size_t		next_count = 4;
	iou_ops_t	*t = NULL;
	_iou_op_t	*_op;

	assert(ops);

	/* look through the available ops list for a free one */
	if (*ops) {
		next_count = (*ops)->count * 2;

		for (t = *ops; t && !t->free; t = t->next);

		if (t && !t->free)
			t = NULL;
	}

	/* no currently free one, add more ops */
	if (!t) {
		t = calloc(1, sizeof(iou_ops_t) + sizeof(_iou_op_t) * next_count);
		if (!t)
			return NULL;

		t->count = next_count;
		for (int i = 0; i < next_count; i++) {
			t->ops[i].container = t;
			t->ops[i].free_next = t->free;
			t->free = &t->ops[i];
		}

		t->next = *ops;
		*ops = t;
	}

	_op = t->free;
	t->free = _op->free_next;
	_op->free_next = NULL;

	return _op;
}


static void ops_put(_iou_op_t *_op)
{
	assert(_op);
	assert(_op->container);

	_op->free_next = _op->container->free;
	_op->container->free = _op;
}


iou_t * iou_new(unsigned entries)
{
	iou_t	*iou;

	assert(entries);

	iou = calloc(1, sizeof(*iou));
	if (!iou)
		return NULL;

	if (io_uring_queue_init(entries, &iou->ring, 0) < 0) {
		free(iou);
		return NULL;
	}

	return iou;
}


iou_t * iou_free(iou_t *iou)
{
	if (iou) {
		iou_ops_t	*t;

		while ((t = iou->ops)) {
			iou->ops = t->next;
			free(t);
		}

		io_uring_queue_exit(&iou->ring);
		free(iou);
	}

	return NULL;
}


/* allocates an op, which wraps an io_uring sqe, result, and associated closure */
/* when an op completes, its result member is populated from the cqe, and its
 * closure supplied when queued is called.   If the closure must access the cqe,
 * it should bind the op by the submission time, then it may access the
 * op->cqe member.
 */
iou_op_t * iou_op_new(iou_t *iou)
{
	_iou_op_t	*_op;

	assert(iou);

	_op = ops_get(&iou->ops);
	if (!_op)
		return NULL;

	_op->public.sqe = io_uring_get_sqe(&iou->ring);
	if (!_op->public.sqe) {
		ops_put(_op);
		return NULL;
	}

	iou->n_issued++;

	return &_op->public;
}


/* finalizes op for submission, assigning cb+cb_data to op,
 * and assigning op to op->sqe->user_data.
 *
 * op shouldn't be manipulated again after submission, cb_data
 * must be valid at least until op completes and calles cb.
 *
 * if cb returns a negative value it's treated as a hard error ending and
 * propagating out of iou_run().
 */
void iou_op_queue(iou_t *iou, iou_op_t *op, int (*cb)(void *cb_data), void *cb_data)
{
	_iou_op_t	*_op;

	assert(iou);
	assert(op);
	assert(cb);
	assert(iou->n_issued);

	_op = CONTAINER_OF(op, _iou_op_t, public);

	_op->cb = cb;
	_op->cb_data = cb_data;

	io_uring_sqe_set_data(op->sqe, _op);

	iou->n_issued--;
	iou->n_queued++;
}


/* If there's more queued submissions, submit them to io_uring.
 *
 * XXX: This is made public under the assumption that there's utility in a
 * synchronous flushing of the SQ.  I originally added it in an attempt to pin
 * the dirfd supplied with a batch of relative OPENAT requests, so the dirfd
 * could be closed immediately rather than having to refcount the dependent
 * dirfd in the caller for deferred closing until the dependent ops finish.  At
 * the time it didn't work - the OPENATs failed with EBADFD because of the
 * immediate dirfd closing, even with the flush before the close.  But
 * discussion on the io-uring mailing list left me optimistic it'd get fixed
 * eventually, so keeping it public.  There might already be other operations
 * where flushing is effective, I haven't dug into the kernel implementation.
 */
int iou_flush(iou_t *iou)
{
	int	r;

	assert(iou);

	if (!iou->n_queued)
		return 0;

	/* defend against any issued but non-queued SQEs at this point,
	 * since io_uring_submit() would submit those but they're incomplete.
	 */
	assert(!iou->n_issued);

	r = io_uring_submit(&iou->ring);
	if (r < 0)
		return r;

	assert(r <= iou->n_queued);

	iou->n_queued -= r;
	iou->n_submitted += r;

	return 0;
}


/* run an iou scheduler until quit, or an error occurs */
/* returns -errno on error, 0 on a graceful finish */
/* XXX: note if no op has been queued to kickstart things, this is effectively a noop. */
int iou_run(iou_t *iou)
{
	assert(iou);

	while (!iou->quit && (iou->n_queued + iou->n_submitted)) {
		struct io_uring_cqe	*cqe;
		_iou_op_t		*_op;
		int			r;

		/* if there's more queued submissions, submit them. */
		r = iou_flush(iou);
		if (r < 0)
			return r;

		/* wait for and process a completion */
		r = io_uring_wait_cqe(&iou->ring, &cqe);
		if (r < 0)
			return r;

		_op = io_uring_cqe_get_data(cqe);
		_op->public.result = cqe->res;

		io_uring_cqe_seen(&iou->ring, cqe);
		iou->n_submitted--;

		r = _op->cb(_op->cb_data);
		if (r < 0)
			return r;

		ops_put(_op);
	}

	return 0;
}


/* Inform the supplied iou instance to quit its event loop gracefully */
int iou_quit(iou_t *iou)
{
	assert(iou);

	iou->quit = 1;

	return 0;
}


/* Resize the underlying io_uring to entries.
 *
 * This can only be done when there's nothing queued or submitted, since
 * it's essentially tearing down and recreating everything including the
 * queues containing such entries.
 *
 * The intended purpose of this is sometimes you start out with a safe
 * minimum of queue depth for a bootstrap process, but in the course of
 * bootstrap you learn necessary depths to accomodate subsequent operations
 * and need to resize accordingly.
 */
int iou_resize(iou_t *iou, unsigned entries)
{
	assert(iou);
	assert(entries);
	assert(!(iou->n_issued + iou->n_queued + iou->n_submitted));

	io_uring_queue_exit(&iou->ring);

	return io_uring_queue_init(entries, &iou->ring, 0);
}


/* Accessor for getting at the underlying io_uring struct for calling
 * liburing helpers directly against, use with care.
 */
struct io_uring * iou_ring(iou_t *iou)
{
	assert(iou);

	return &iou->ring;
}
