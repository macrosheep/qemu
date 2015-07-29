/*
 * Copyright (c) 2015 FUJITSU LIMITED
 * Author: Yang Hongyang <yanghy@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "net/filter.h"
#include "net/queue.h"
#include "filters.h"
#include "qemu-common.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/iov.h"

typedef struct FILTERBUFFERState {
    NetFilterState nf;
    NetQueue *incoming_queue;
    NetQueue *inflight_queue;
    QEMUBH *flush_bh;
    int64_t interval;
    QEMUTimer release_timer;
} FILTERBUFFERState;

static void packet_send_completed(NetClientState *nc, ssize_t len)
{
    return;
}

static void filter_buffer_flush(NetFilterState *nf)
{
    FILTERBUFFERState *s = DO_UPCAST(FILTERBUFFERState, nf, nf);
    NetQueue *queue = s->inflight_queue;
    NetPacket *packet;

    while (queue && !QTAILQ_EMPTY(&queue->packets)) {
        packet = QTAILQ_FIRST(&queue->packets);
        QTAILQ_REMOVE(&queue->packets, packet, entry);
        queue->nq_count--;

        qemu_net_queue_send(packet->sender->peer->incoming_queue,
                            packet->sender,
                            packet->flags,
                            packet->data,
                            packet->size,
                            packet->sent_cb);

        /*
         * now that we pass the packet to sender->peer->incoming_queue, we
         * don't care the reture value here, because the peer's queue will
         * take care of this packet
         */
        g_free(packet);
    }

    g_free(queue);
    s->inflight_queue = NULL;
}

static void filter_buffer_flush_bh(void *opaque)
{
    FILTERBUFFERState *s = opaque;
    NetFilterState *nf = &s->nf;
    filter_buffer_flush(nf);
}

static void filter_buffer_release_one(NetFilterState *nf)
{
    FILTERBUFFERState *s = DO_UPCAST(FILTERBUFFERState, nf, nf);

    /* flush inflight packets */
    if (s->inflight_queue) {
        filter_buffer_flush(nf);
    }

    s->inflight_queue = s->incoming_queue;
    s->incoming_queue = qemu_new_net_queue(nf);
    qemu_bh_schedule(s->flush_bh);
}

static void filter_buffer_release_timer(void *opaque)
{
    FILTERBUFFERState *s = opaque;
    filter_buffer_release_one(&s->nf);
    timer_mod(&s->release_timer,
              qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + s->interval);
}

/* filter APIs */
static ssize_t filter_buffer_receive_iov(NetFilterState *nf,
                                         NetClientState *sender,
                                         unsigned flags,
                                         const struct iovec *iov,
                                         int iovcnt)
{
    FILTERBUFFERState *s = DO_UPCAST(FILTERBUFFERState, nf, nf);
    NetQueue *queue = s->incoming_queue;

    qemu_net_queue_append_iov(queue, sender, flags, iov, iovcnt,
                              packet_send_completed);
    return iov_size(iov, iovcnt);
}

static void filter_buffer_cleanup(NetFilterState *nf)
{
    FILTERBUFFERState *s = DO_UPCAST(FILTERBUFFERState, nf, nf);

    if (s->interval) {
        timer_del(&s->release_timer);
    }

    /* flush inflight packets */
    filter_buffer_flush(nf);
    /* flush incoming packets */
    s->inflight_queue = s->incoming_queue;
    s->incoming_queue = NULL;
    filter_buffer_flush(nf);

    if (s->flush_bh) {
        qemu_bh_delete(s->flush_bh);
        s->flush_bh = NULL;
    }
    return;
}


static NetFilterInfo net_filter_buffer_info = {
    .type = NET_FILTER_OPTIONS_KIND_BUFFER,
    .size = sizeof(FILTERBUFFERState),
    .receive_iov = filter_buffer_receive_iov,
    .cleanup = filter_buffer_cleanup,
};

int net_init_filter_buffer(const NetFilterOptions *opts, const char *name,
                           int chain, NetClientState *netdev, Error **errp)
{
    NetFilterState *nf;
    FILTERBUFFERState *s;
    const NetFilterBufferOptions *bufferopt;

    assert(opts->kind == NET_FILTER_OPTIONS_KIND_BUFFER);
    bufferopt = opts->buffer;

    nf = qemu_new_net_filter(&net_filter_buffer_info,
                             netdev, "buffer", name, chain);
    s = DO_UPCAST(FILTERBUFFERState, nf, nf);
    s->incoming_queue = qemu_new_net_queue(nf);
    s->flush_bh = qemu_bh_new(filter_buffer_flush_bh, s);
    s->interval = bufferopt->has_interval ? bufferopt->interval : 0;
    if (s->interval) {
        timer_init_us(&s->release_timer, QEMU_CLOCK_VIRTUAL,
                      filter_buffer_release_timer, s);
        timer_mod(&s->release_timer,
                  qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + s->interval);
    }

    return 0;
}
