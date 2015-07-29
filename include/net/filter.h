/*
 * Copyright (c) 2015 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#ifndef QEMU_NET_FILTER_H
#define QEMU_NET_FILTER_H

#include "qemu-common.h"
#include "qemu/typedefs.h"

/* the netfilter chain */
enum {
    NET_FILTER_IN,
    NET_FILTER_OUT,
    NET_FILTER_ALL,
};

typedef void (FilterCleanup) (NetFilterState *);
/*
 * Return:
 *   0: finished handling the packet, we should continue
 *   size: filter stolen this packet, we stop pass this packet further
 */
typedef ssize_t (FilterReceiveIOV)(NetFilterState *, NetClientState *sender,
                                   unsigned flags, const struct iovec *, int);

typedef struct NetFilterInfo {
    NetFilterOptionsKind type;
    size_t size;
    FilterCleanup *cleanup;
    FilterReceiveIOV *receive_iov;
} NetFilterInfo;

struct NetFilterState {
    NetFilterInfo *info;
    char *model;
    char *name;
    NetClientState *netdev;
    int chain;
    QTAILQ_ENTRY(NetFilterState) next;
};

int net_init_filters(void);
NetFilterState *qemu_new_net_filter(NetFilterInfo *info,
                                    NetClientState *netdev,
                                    const char *model,
                                    const char *name,
                                    int chain);

#endif /* QEMU_NET_FILTER_H */
