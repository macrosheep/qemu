/*
 * Copyright (c) 2015 FUJITSU LIMITED
 * Author: Yang Hongyang <yanghy@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu-common.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"

#include "net/filter.h"
#include "net/net.h"
#include "net/vhost_net.h"
#include "qom/object_interfaces.h"
#include "qemu/iov.h"

static QTAILQ_HEAD(, NetFilterState) net_filters;

ssize_t qemu_netfilter_pass_to_next(NetClientState *sender,
                                    unsigned flags,
                                    const struct iovec *iov,
                                    int iovcnt,
                                    void *opaque)
{
    int ret = 0;
    int chain;
    NetFilterState *nf = opaque;
    NetFilterState *next = QTAILQ_NEXT(nf, next);

    if (!sender || !sender->peer) {
        /* no receiver, or sender been deleted, no need to pass it further */
        goto out;
    }

    if (nf->chain == NET_FILTER_CHAIN_ALL) {
        if (sender == nf->netdev) {
            /* This packet is sent by netdev itself */
            chain = NET_FILTER_CHAIN_OUT;
        } else {
            chain = NET_FILTER_CHAIN_IN;
        }
    } else {
        chain = nf->chain;
    }

    while (next) {
        if (next->chain == chain || next->chain == NET_FILTER_CHAIN_ALL) {
            /*
             * if qemu_netfilter_pass_to_next been called, means that
             * the packet has been hold by filter and has already retured size
             * to the sender, so sent_cb shouldn't be called later, just
             * pass NULL to next.
             */
            ret = NETFILTER_GET_CLASS(OBJECT(next))->receive_iov(
                                    next, sender, flags, iov, iovcnt, NULL);
            if (ret) {
                return ret;
            }
        }
        next = QTAILQ_NEXT(next, next);
    }

    /*
     * We have gone through all filters, pass it to receiver.
     * Do the valid check again incase sender or receiver been
     * deleted while we go through filters.
     */
    if (sender && sender->peer) {
        return qemu_net_queue_send_iov(sender->peer->incoming_queue,
                                       sender, flags, iov, iovcnt, NULL);
    }

out:
    /* no receiver, or sender been deleted */
    return iov_size(iov, iovcnt);
}

static char *netfilter_get_netdev_id(Object *obj, Error **errp)
{
    NetFilterState *nf = NETFILTER(obj);

    return g_strdup(nf->netdev_id);
}

static void netfilter_set_netdev_id(Object *obj, const char *str, Error **errp)
{
    NetFilterState *nf = NETFILTER(obj);

    nf->netdev_id = g_strdup(str);
}

static int netfilter_get_chain(Object *obj, Error **errp G_GNUC_UNUSED)
{
    NetFilterState *nf = NETFILTER(obj);
    return nf->chain;
}

static void netfilter_set_chain(Object *obj, int chain, Error **errp)
{
    NetFilterState *nf = NETFILTER(obj);
    nf->chain = chain;
}

static void netfilter_init(Object *obj)
{
    object_property_add_str(obj, "netdev",
                            netfilter_get_netdev_id, netfilter_set_netdev_id,
                            NULL);
    object_property_add_enum(obj, "chain", "NetFilterChain",
                             NetFilterChain_lookup,
                             netfilter_get_chain, netfilter_set_chain,
                             NULL);
}

static void netfilter_cleanup(Object *obj)
{
    NetFilterState *nf = NETFILTER(obj);
    NetFilterClass *nfc = NETFILTER_GET_CLASS(obj);

    if (nfc->cleanup) {
        nfc->cleanup(nf);
    }

    if (nf->netdev && !QTAILQ_EMPTY(&nf->netdev->filters)) {
        QTAILQ_REMOVE(&nf->netdev->filters, nf, next);
    }
    if (!QTAILQ_EMPTY(&net_filters)) {
        QTAILQ_REMOVE(&net_filters, nf, global_list);
    }

    g_free(nf->name);
}

static void netfilter_complete(UserCreatable *uc, Error **errp)
{
    NetFilterState *nf = NETFILTER(uc);
    NetClientState *ncs[MAX_QUEUE_NUM];
    NetFilterClass *nfc = NETFILTER_GET_CLASS(uc);
    int queues;
    Error *local_err = NULL;

    if (!nf->netdev_id) {
        error_setg(errp, "Parameter 'netdev' is required");
        return;
    }

    queues = qemu_find_net_clients_except(nf->netdev_id, ncs,
                                          NET_CLIENT_OPTIONS_KIND_NIC,
                                          MAX_QUEUE_NUM);
    if (queues < 1) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "netdev",
                   "a network backend id");
        return;
    } else if (queues > 1) {
        error_setg(errp, "Multi queue is not supported");
        return;
    }

    if (get_vhost_net(ncs[0])) {
        error_setg(errp, "Vhost is not supported");
        return;
    }

    QTAILQ_INIT(&net_filters);
    nf->name = object_get_canonical_path_component(OBJECT(nf));
    nf->netdev = ncs[0];

    if (nfc->setup) {
        nfc->setup(nf, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }
    QTAILQ_INSERT_TAIL(&net_filters, nf, global_list);
    QTAILQ_INSERT_TAIL(&nf->netdev->filters, nf, next);
}

static bool netfilter_can_be_deleted(UserCreatable *uc, Error **errp)
{
    return true;
}

static void netfilter_class_init(ObjectClass *oc, void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = netfilter_complete;
    ucc->can_be_deleted = netfilter_can_be_deleted;
}

static const TypeInfo netfilter_info = {
    .name = TYPE_NETFILTER,
    .parent = TYPE_OBJECT,
    .abstract = true,
    .class_size = sizeof(NetFilterClass),
    .class_init = netfilter_class_init,
    .instance_size = sizeof(NetFilterState),
    .instance_init = netfilter_init,
    .instance_finalize = netfilter_cleanup,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void register_types(void)
{
    type_register_static(&netfilter_info);
}

type_init(register_types);
