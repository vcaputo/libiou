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

#ifndef _IOU_H
#define _IOU_H

#include <liburing.h>

typedef struct iou_t iou_t;

typedef struct iou_op_t {
	struct io_uring_sqe	*sqe;
	int			result;
} iou_op_t;


iou_t * iou_new(unsigned entries);
iou_t * iou_free(iou_t *iou);
iou_op_t * iou_op_new(iou_t *iou);
void iou_op_queue(iou_t *iou, iou_op_t *op, int (*cb)(void *cb_data), void *cb_data);
int iou_flush(iou_t *iou);
int iou_run(iou_t *iou);
int iou_quit(iou_t *iou);
int iou_resize(iou_t *iou, unsigned entries);
struct io_uring * iou_ring(iou_t *iou);
int iou_async(iou_t *iou, int (*async_cb)(void *async_cb_data), void *async_cb_data, int (*completion_cb)(void *completion_cb_data), void *completion_cb_data);

#endif
