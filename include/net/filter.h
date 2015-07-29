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

typedef struct NetFilterInfo {
    NetFilterOptionsKind type;
    size_t size;
    FilterCleanup *cleanup;
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

#endif /* QEMU_NET_FILTER_H */
