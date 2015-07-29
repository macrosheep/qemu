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

typedef struct FILTERBUFFERState {
    NetFilterState nf;
    NetClientState dummy; /* used to send buffered packets */
    NetQueue *incoming_queue;
    NetQueue *inflight_queue;
    QEMUBH *flush_bh;
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

        if (packet->flags & QEMU_NET_PACKET_FLAG_RAW) {
            qemu_send_packet_raw(&s->dummy, packet->data, packet->size);
        } else {
            qemu_send_packet_async(&s->dummy, packet->data, packet->size,
                                   packet->sent_cb);
        }

        /*
         * now that we pass the packet to sender->peer->incoming_queue, we
         * don't care the reture value here, because the peer's queue will
         * take care of this packet
         */
        g_free(packet);
    }

    if (queue && QTAILQ_EMPTY(&queue->packets)) {
        g_free(queue);
        s->inflight_queue = NULL;
    }
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

/* filter APIs */
static ssize_t filter_buffer_receive(NetFilterState *nf,
                                     NetClientState *sender,
                                     unsigned flags,
                                     const uint8_t *data,
                                     size_t size)
{
    FILTERBUFFERState *s = DO_UPCAST(FILTERBUFFERState, nf, nf);
    NetQueue *queue = s->incoming_queue;

    if (!sender->info) {
        /* This must be a dummy NetClientState, do nothing */
        return 0;
    }

    if (sender->info->type == NET_CLIENT_OPTIONS_KIND_NIC) {
        /* we only buffer guest output packets */
        qemu_net_queue_append(queue, sender, flags, data, size,
                              packet_send_completed);
        /* Now that we have buffered the packet, return sucess */
        return size;
    }

    return 0;
}

static void filter_buffer_cleanup(NetFilterState *nf)
{
    FILTERBUFFERState *s = DO_UPCAST(FILTERBUFFERState, nf, nf);

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
    .receive = filter_buffer_receive,
    .cleanup = filter_buffer_cleanup,
};

int net_init_filter_buffer(const NetFilterOptions *opts, const char *name,
                           NetClientState *netdev, Error **errp)
{
    NetFilterState *nf;
    FILTERBUFFERState *s;

    assert(opts->kind == NET_FILTER_OPTIONS_KIND_BUFFER);

    nf = qemu_new_net_filter(&net_filter_buffer_info, netdev, "buffer", name);
    s = DO_UPCAST(FILTERBUFFERState, nf, nf);
    /*
     * we need the dummy NetClientState to send packets in order to avoid
     * receive packets again.
     * we are buffering guest output packets, our buffered packets should be
     * sent to real network backend, so dummy's peer should be that backend.
     */
    s->dummy.peer = netdev;
    s->incoming_queue = qemu_new_net_queue(nf);
    s->flush_bh = qemu_bh_new(filter_buffer_flush_bh, s);

    return 0;
}

/* public APIs */
void filter_buffer_release_all(void)
{
    NetFilterState *nfs[MAX_QUEUE_NUM];
    int queues, i;

    queues = qemu_find_netfilters_by_model("buffer", nfs, MAX_QUEUE_NUM);

    for (i = 0; i < queues; i++) {
        filter_buffer_release_one(nfs[i]);
    }
}
