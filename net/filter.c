/*
 * Copyright (c) 2015 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu-common.h"
#include "qapi-visit.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "qapi-visit.h"
#include "qapi/opts-visitor.h"
#include "qapi/dealloc-visitor.h"
#include "qemu/config-file.h"

#include "net/filter.h"
#include "net/net.h"

static QTAILQ_HEAD(, NetFilterState) net_filters;

NetFilterState *qemu_new_net_filter(NetFilterInfo *info,
                                    NetClientState *netdev,
                                    const char *name,
                                    int chain)
{
    NetFilterState *nf;

    assert(info->size >= sizeof(NetFilterState));
    assert(info->receive_iov);

    nf = g_malloc0(info->size);
    nf->info = info;
    nf->name = g_strdup(name);
    nf->netdev = netdev;
    nf->chain = chain;
    QTAILQ_INSERT_TAIL(&net_filters, nf, global_list);
    QTAILQ_INSERT_TAIL(&netdev->filters, nf, next);

    return nf;
}

static inline void qemu_cleanup_net_filter(NetFilterState *nf)
{
    QTAILQ_REMOVE(&nf->netdev->filters, nf, next);
    QTAILQ_REMOVE(&net_filters, nf, global_list);

    if (nf->info->cleanup) {
        nf->info->cleanup(nf);
    }

    g_free(nf->name);
    g_free(nf);
}

typedef int (NetFilterInit)(const NetFilterOptions *opts,
                            const char *name, int chain,
                            NetClientState *netdev, Error **errp);

static
NetFilterInit * const net_filter_init_fun[NET_FILTER_OPTIONS_KIND_MAX] = {
};

static int net_filter_init1(const NetFilter *netfilter, Error **errp)
{
    NetClientState *netdev = NULL;
    NetClientState *ncs[MAX_QUEUE_NUM];
    const char *name = netfilter->id;
    const char *netdev_id = netfilter->netdev;
    const char *chain_str = NULL;
    const NetFilterOptions *opts = netfilter->opts;
    int chain, queues;

    if (!net_filter_init_fun[opts->kind]) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "type",
                   "a net filter type");
        return -1;
    }

    if (netfilter->has_chain) {
        chain_str = netfilter->chain;
        if (!strcmp(chain_str, "in")) {
            chain = NET_FILTER_IN;
        } else if (!strcmp(chain_str, "out")) {
            chain = NET_FILTER_OUT;
        } else if (!strcmp(chain_str, "all")) {
            chain = NET_FILTER_ALL;
        } else {
            error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "chain",
                       "netfilter chain (in/out/all)");
            return -1;
        }
    } else {
        /* default */
        chain = NET_FILTER_ALL;
    }

    queues = qemu_find_net_clients_except(netdev_id, ncs,
                                          NET_CLIENT_OPTIONS_KIND_NIC,
                                          MAX_QUEUE_NUM);
    if (queues > 1) {
        error_setg(errp, "multiqueues is not supported by now");
        return -1;
    } else if (queues < 1) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "netdev",
                   "a network backend id");
        return -1;
    }

    netdev = ncs[0];

    if (net_filter_init_fun[opts->kind](opts, name, chain, netdev, errp) < 0) {
        if (errp && !*errp) {
            error_setg(errp, QERR_DEVICE_INIT_FAILED,
                       NetFilterOptionsKind_lookup[opts->kind]);
        }
        return -1;
    }

    return 0;
}

static int net_init_filter(void *dummy, QemuOpts *opts, Error **errp)
{
    NetFilter *object = NULL;
    Error *err = NULL;
    int ret = -1;
    OptsVisitor *ov = opts_visitor_new(opts);

    visit_type_NetFilter(opts_get_visitor(ov), &object, NULL, &err);
    opts_visitor_cleanup(ov);

    if (!err) {
        ret = net_filter_init1(object, &err);
    }

    if (object) {
        QapiDeallocVisitor *dv = qapi_dealloc_visitor_new();

        visit_type_NetFilter(qapi_dealloc_get_visitor(dv), &object, NULL, NULL);
        qapi_dealloc_visitor_cleanup(dv);
    }

    error_propagate(errp, err);
    return ret;
}

int net_init_filters(void)
{
    QTAILQ_INIT(&net_filters);

    if (qemu_opts_foreach(qemu_find_opts("netfilter"),
                          net_init_filter, NULL, NULL)) {
        return -1;
    }

    return 0;
}

QemuOptsList qemu_netfilter_opts = {
    .name = "netfilter",
    .implied_opt_name = "type",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_netfilter_opts.head),
    .desc = {
        /*
         * no elements => accept any params
         * validation will happen later
         */
        { /* end of list */ }
    },
};
