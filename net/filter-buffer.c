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
#include "qemu/timer.h"
#include "qemu/iov.h"

typedef struct FILTERBUFFERState {
    NetFilterState nf;
    NetQueue *incoming_queue;
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
    NetQueue *queue = s->incoming_queue;
    NetPacket *packet;

    while (queue && !QTAILQ_EMPTY(&queue->packets)) {
        packet = QTAILQ_FIRST(&queue->packets);
        QTAILQ_REMOVE(&queue->packets, packet, entry);
        queue->nq_count--;

        if (packet->sender && packet->sender->peer) {
            qemu_netfilter_pass_to_next(nf, packet->sender, packet->flags,
                                        packet->data, packet->size);
        }

        /*
         * now that we pass the packet to next filter, we don't care the
         * reture value here, because the filter layer or other filter
         * will take care of this packet
         */
        g_free(packet);
    }
}

static void filter_buffer_release_timer(void *opaque)
{
    FILTERBUFFERState *s = opaque;
    filter_buffer_flush(&s->nf);
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

    /* flush packets */
    filter_buffer_flush(nf);
    g_free(s->incoming_queue);
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
    s->interval = bufferopt->has_interval ? bufferopt->interval : 0;
    if (s->interval) {
        timer_init_us(&s->release_timer, QEMU_CLOCK_VIRTUAL,
                      filter_buffer_release_timer, s);
        timer_mod(&s->release_timer,
                  qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + s->interval);
    }

    return 0;
}
