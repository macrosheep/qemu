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

typedef void (FilterCleanup) (NetFilterState *);
/*
 * Return:
 *   0: finished handling the packet, we should continue
 *   size: filter stolen this packet, we stop pass this packet further
 */
typedef ssize_t (FilterReceive)(NetFilterState *, NetClientState *sender,
                                unsigned flags, const uint8_t *, size_t);
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
    FilterReceive *receive;
    FilterReceiveIOV *receive_iov;
} NetFilterInfo;

struct NetFilterState {
    NetFilterInfo *info;
    char *model;
    char *name;
    NetClientState *netdev;
    QTAILQ_ENTRY(NetFilterState) next;
};

int net_init_filters(void);
NetFilterState *qemu_new_net_filter(NetFilterInfo *info,
                                    NetClientState *netdev,
                                    const char *model,
                                    const char *name);
void netfilter_add(QemuOpts *opts, Error **errp);
void qmp_netfilter_add(QDict *qdict, QObject **ret, Error **errp);
int qemu_find_netfilters_by_model(const char *model, NetFilterState **nfs,
                                  int max);

/* netbuffer filter */
void filter_buffer_release_all(void);

#endif /* QEMU_NET_FILTER_H */
