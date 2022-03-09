/*
 *  Copyright (C) 2020-2022 - Vito Caputo - <vcaputo@pengaru.com>
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
#include <pthread.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/prctl.h>

#include "iou.h"

/* iou is a minimal IO-oriented async callback scheduler built atop io_uring */

#define CQE_BATCH_SIZE	16

typedef struct _iou_op_t _iou_op_t;
typedef struct iou_ops_t iou_ops_t;

typedef struct iou_thread_t {
	pthread_t	pthread;
	iou_t		*iou;
} iou_thread_t;

typedef struct iou_t {
	struct io_uring	ring;
	unsigned	n_issued,	/* SQEs allocated, but no data set */
			n_queued,	/* SQEs allocated+data set */
			n_submitted,	/* SQEs submitted for processing and not yet "seen" for completion */
			n_async;	/* async ops created and not yet completed by iou_run() */
	_iou_op_t	*processed;	/* async work processed but waiting for completion */
	pthread_mutex_t	processed_mutex;/* serializes processed list accesses */
	unsigned	quit:1;
	iou_ops_t	*ops;

	struct {
		_iou_op_t	*head, *tail;
		pthread_cond_t	cond;
		pthread_mutex_t	mutex;	/* serializes {head,tail} */
	} async;

	int		n_threads;
	iou_thread_t	threads[];
} iou_t;

/* private container of the public iou_op_t */
struct _iou_op_t {
	iou_op_t	public;

	int		(*cb)(void *cb_data);
	void		*cb_data;

	/* this is added for the iou_async()-submitted threaded processing,
	 * which does add some bloat to all ops and might warrant creating a
	 * distinct async op type with its own allocation/free lists... TODO
	 */
	int		(*async_cb)(void *async_cb_data);
	void		*async_cb_data;

	_iou_op_t	*next;
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
			t->ops[i].next = t->free;
			t->free = &t->ops[i];
		}

		t->next = *ops;
		*ops = t;
	}

	_op = t->free;
	t->free = _op->next;
	_op->next = NULL;

	return _op;
}


static void ops_put(_iou_op_t *_op)
{
	assert(_op);
	assert(_op->container);

	_op->next = _op->container->free;
	_op->container->free = _op;
}


/* CPU-bound async work thread, created @ iou_new(), process iou_async() callbacks */
static void * iou_thread(iou_thread_t *thread)
{
	iou_t	*iou;

	assert(thread);
	assert(thread->iou);

	prctl(PR_SET_NAME, "libiou-async");
	iou = thread->iou;

	for (;;) {
		_iou_op_t	*_op;

		pthread_cleanup_push((void (*)(void *))pthread_mutex_unlock, &iou->async.mutex);
		pthread_mutex_lock(&iou->async.mutex);
		while (!iou->async.head)
			pthread_cond_wait(&iou->async.cond, &iou->async.mutex);

		_op = iou->async.head;
		iou->async.head = _op->next;
		if (!_op->next)
			iou->async.tail = NULL;
		pthread_cleanup_pop(1);

		assert(_op->async_cb(_op->async_cb_data) >= 0); /* XXX treating errors here as fatal disasters for now */

		/* now the work is handed back to iou_run() via processed_list, which
		 * iou_run() polls after io_uring submissions but before waiting for io completions.
		 */
		pthread_cleanup_push((void (*)(void *))pthread_mutex_unlock, &iou->processed_mutex);
		pthread_mutex_lock(&iou->processed_mutex);
		/* FIXME: this should probably get added to a processed_tail to roughly preserve the completion order */
		_op->next = iou->processed;
		iou->processed = _op;
		pthread_cleanup_pop(1);
	}

	pthread_exit(NULL);
}


iou_t * iou_new(unsigned entries)
{
	int	n_threads = 2;	/* TODO: size according to of cores probed @ runtime */
	iou_t	*iou;

	assert(entries);

	iou = calloc(1, sizeof(*iou) + sizeof(iou->threads[0]) * n_threads);
	if (!iou)
		return NULL;

	if (io_uring_queue_init(entries, &iou->ring, 0) < 0) {
		free(iou);
		return NULL;
	}

	/* TODO: handle failures */
	pthread_mutex_init(&iou->processed_mutex, NULL);
	pthread_cond_init(&iou->async.cond, NULL);
	pthread_mutex_init(&iou->async.mutex, NULL);
	for (int i = 0; i < n_threads; i++) {
		iou->threads[i].iou = iou;
		pthread_create(&iou->threads[i].pthread, NULL, (void *(*)(void *))iou_thread, &iou->threads[i]);
	}
	iou->n_threads = n_threads;

	return iou;
}


iou_t * iou_free(iou_t *iou)
{
	if (iou) {
		iou_ops_t	*t;

		for (int i = 0; i < iou->n_threads; i++)
			pthread_cancel(iou->threads[i].pthread);

		for (int i = 0; i < iou->n_threads; i++)
			pthread_join(iou->threads[i].pthread, NULL);

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
 * callback supplied when queued is called with its accompanying
 * cb_data pointer.  Callback access to the raw cqe isn't currently
 * facilitated.
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

	while (!iou->quit && (iou->n_queued + iou->n_submitted + iou->n_async)) {
		_iou_op_t	*_op;
		int		r;

		/* flush any queued iou_ops to get those IO balls rolling. */
		r = iou_flush(iou);
		if (r < 0)
			return r;

		/* complete any processed async work */
		if (iou->n_async) {

			pthread_mutex_lock(&iou->processed_mutex);
			_op = iou->processed;
			iou->processed = NULL;
			pthread_mutex_unlock(&iou->processed_mutex);

			while (_op) {
				_iou_op_t	*next = _op->next;

				iou->n_async--;

				r = _op->cb(_op->cb_data);
				if (r < 0)
					return r;

				ops_put(_op);
				_op = next;
			}
		}

		if (!iou->n_submitted)
			continue;

		{
			struct io_uring_cqe	*cqes[CQE_BATCH_SIZE];
			unsigned		n;

			/* optimistically try get a batch first */
			n = io_uring_peek_batch_cqe(&iou->ring, cqes, CQE_BATCH_SIZE);
			if (!n) {
				/* Bummer, wait for at least one. */
				r = io_uring_wait_cqe(&iou->ring, cqes);
				if (r < 0)
					return r;
				n = 1;
			}

			for (unsigned i = 0; i < n; i++) {
				struct io_uring_cqe	*cqe = cqes[i];

				_op = io_uring_cqe_get_data(cqe);
				_op->public.result = cqe->res;

				io_uring_cqe_seen(&iou->ring, cqe);
				iou->n_submitted--;

				r = _op->cb(_op->cb_data);
				if (r < 0)
					return r;

				ops_put(_op);
			}
		}
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


/* Resize the underlying io_uring to n_entries.
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
int iou_resize(iou_t *iou, unsigned n_entries)
{
	assert(iou);
	assert(n_entries);
	assert(!(iou->n_issued + iou->n_queued + iou->n_submitted));

	io_uring_queue_exit(&iou->ring);

	return io_uring_queue_init(n_entries, &iou->ring, 0);
}


/* Accessor for getting at the underlying io_uring struct for calling
 * liburing helpers directly against, use with care.
 */
struct io_uring * iou_ring(iou_t *iou)
{
	assert(iou);

	return &iou->ring;
}


/* create an async CPU-bound work unit which runs async_sb on a worker thread, eventually submitting an IORING_OP_NOP iou_op w/completion_cb+completion_cb_data when
 * async_cb(async_cb_data) returns from the thread.
 */
int iou_async(iou_t *iou, int (*async_cb)(void *async_cb_data), void *async_cb_data, int (*completion_cb)(void *completion_cb_data), void *completion_cb_data)
{
	_iou_op_t	*_op;

	assert(iou);
	assert(async_cb);
	assert(completion_cb);

	/* reuse iou_op_t to encapsulate async work, eventually it gets submitted with an sqe for the NOP but
	 * the sqe isn't allocated+submitted until after the CPU-bound work is completed from the iou_thread.
	 */
	_op = ops_get(&iou->ops);
	if (!_op)
		return -ENOMEM;

	_op->cb = completion_cb;
	_op->cb_data = completion_cb_data;
	_op->async_cb = async_cb;
	_op->async_cb_data = async_cb_data;
	_op->next = NULL;

	iou->n_async++;

	pthread_mutex_lock(&iou->async.mutex);
	if (iou->async.tail)
		iou->async.tail->next = _op;
	else
		iou->async.head = _op;
	iou->async.tail = _op;

	pthread_cond_signal(&iou->async.cond);
	pthread_mutex_unlock(&iou->async.mutex);

	return 0;
}
