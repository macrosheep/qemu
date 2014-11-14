/*
 *  COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 *  (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (C) 2014 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */
#include <migration/migration-colo.h>

#ifndef DISABLE_HEARTBEAT

#include <heartbeat/ha_msg.h>
#include <clplumbing/cl_log.h>
#include <heartbeat/hb_api.h>
#include <migration/migration.h>
#include <migration/migration-failover.h>
#include <block/aio.h>

#define DEBUG_COLO

#ifdef DEBUG_COLO
#define DPRINTF(fmt, ...) \
    do { printf("COLO: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

typedef struct heartbeat_node {
    ll_cluster_t *hb;
    char master_id[256]; /* master node name */
    char slave_id[256];  /* slave node name */
    int master_alive;
    int slave_alive;
    int keepalive;
    int deadtime;
    QemuThread *thread;
    QemuMutex mutex;
    int status;
} heartbeat_node_t;

heartbeat_node_t client;
bool vmstate_loading = false;

static int status_isalive(const char *status)
{
    return strcmp(status, "dead") != 0;
}

int get_heartbeat(bool slave)
{
    int heartbeat_alive;
    qemu_mutex_lock(&client.mutex);
    heartbeat_alive = slave == COLO_SIDE_SLAVE ?
                      client.slave_alive : client.master_alive;
    qemu_mutex_unlock(&client.mutex);
    return heartbeat_alive;
}

void set_heartbeat(bool slave, int value)
{
    qemu_mutex_lock(&client.mutex);
    if (slave == COLO_SIDE_SLAVE) {
        client.slave_alive = value;
    } else {
        client.master_alive = value;
    }
    qemu_mutex_unlock(&client.mutex);
}

int heartbeat_deadtime(void)
{
    return client.deadtime;
}

static inline int lookup_node2side(const char *node)
{
    if (!strcmp(node, client.master_id)) {
        return COLO_SIDE_MASTER;
    } else if (!strcmp(node, client.slave_id)) {
        return COLO_SIDE_SLAVE;
    } else {
        return -1;
    }
}

struct hb_arg {
    int stat;
    int side;
    void *p;
    QEMUBH *bh;
};

static void
node_status_update_bh(void *opaque)
{
    struct hb_arg *arg = opaque;
    if (!arg->bh) {
        return ;
    }
    qemu_bh_delete(arg->bh);
    arg->bh = NULL;

    if (!get_heartbeat(arg->side) && client.status == CLIENT_RUNNING) {
        if ((arg->side == COLO_SIDE_SLAVE && colo_is_master()) ||
            (arg->side == COLO_SIDE_MASTER && colo_is_slave())) {
            /* heartbeat request do failover actively */
            DPRINTF("heartbeat request do failover\n");
            if (!failover_request_is_set()) {
                if (vmstate_loading == true && colo_is_slave()) {
                    DPRINTF("incoming thread is loading vmstate\n");
                    vmstate_loading = false;
                    goto out;
                }
                failover_request_set();
                colo_do_failover(NULL);
                /* in slave side, jump to incoming co */
                if (colo_incoming_co) {
                    qemu_coroutine_enter(colo_incoming_co, NULL);
                }
            }
        }
    }

out:
    g_free(arg);
}

static void
node_status_update(const char *node, const char *status, void *opaque)
{
    int stat = status_isalive(status);
    int side = lookup_node2side(node);
    struct hb_arg *arg = g_malloc0(sizeof(*arg));

    arg->stat = stat;
    arg->side = side;
    arg->p = opaque;
    arg->bh = NULL;

    if (side == -1) {
        fprintf(stderr, "ERROR: Node %s is unknown\n", node);
        return;
    }
    set_heartbeat(side, stat);
    arg->bh = qemu_bh_new(node_status_update_bh, arg);
    qemu_bh_schedule(arg->bh);

    DPRINTF("NodeStatus] Status update: Node %s now has status %s\n",
           node, status);
    DPRINTF("%s have status %d\n", client.master_id, client.master_alive);
    DPRINTF("%s have status %d\n", client.slave_id, client.slave_alive);
}

static int heartbeat_client_init(bool ingore);

static void *heartbeat_thread(void *p)
{
    ll_cluster_t *hb;

    DPRINTF("heartbeat info: keepalive %d, deadtime %d\n",
             client.keepalive, client.deadtime);
    DPRINTF("%s have status %d\n", client.master_id, client.master_alive);
    DPRINTF("%s have status %d\n", client.slave_id, client.slave_alive);

    qemu_mutex_lock(&client.mutex);
    while (client.status == CLIENT_RUNNING ||
           client.status == CLIENT_RESTART) {
        if (client.status == CLIENT_RESTART) {
            if (heartbeat_client_init(1)) {
                qemu_mutex_unlock(&client.mutex);
                sleep(1);
                goto retry;
            }
        }
        qemu_mutex_unlock(&client.mutex);

        hb = client.hb;
        while (true) {
            struct ha_msg *msg = NULL;
            msg = hb->llc_ops->readmsg(hb, 1);
            qemu_mutex_lock(&client.mutex);
            if (!colo_is_slave() && !colo_is_master()) {
                /* slave or master have takeover */
                client.status = PARENT_EXIT;
            }
            if (client.status == PARENT_EXIT) {
                qemu_mutex_unlock(&client.mutex);
                if (msg) {
                    ha_msg_del(msg);
                }
                break;
            }
            if (msg == NULL) { /* hearbeat module dead */
                client.status = CLIENT_RESTART;
                qemu_mutex_unlock(&client.mutex);
                break;
            }
            qemu_mutex_unlock(&client.mutex);
            ha_msg_del(msg);
        }

        hb->llc_ops->signoff(hb, TRUE);
        hb->llc_ops->delete(hb);
        client.hb = NULL;
retry:
        qemu_mutex_lock(&client.mutex);
    }
    qemu_mutex_unlock(&client.mutex);

    return NULL;
}

static int heartbeat_client_init(bool ignore)
{
    ll_cluster_t *hb;
    int node_num = 0;
    const char *mynodeid;
    const char *node;

    client.hb = NULL;

    if ((hb = ll_cluster_new("heartbeat")) == NULL) {
        fprintf(stderr, "Cannot create heartbeat client\n");
        return -1;
    }

    if (hb->llc_ops->signon(hb, "colo-heartbeat") != HA_OK) {
        fprintf(stderr, "Cannot sign on with heartbeat\n");
        goto err;
    }

    if (hb->llc_ops->set_nstatus_callback(hb,
                    node_status_update, &client) != HA_OK){
        fprintf(stderr, "Cannot set node status callback\n");
        goto err_signoff;
    }

    mynodeid = hb->llc_ops->get_mynodeid(hb);
    if (hb->llc_ops->init_nodewalk(hb) != HA_OK) {
        fprintf(stderr, "Cannot start node walk\n");
        goto err_signoff;
    }
    while ((node = hb->llc_ops->nextnode(hb)) != NULL) {
        node_num++;
        if ((!strcmp(mynodeid, node) && !colo_is_slave()) ||
            (strcmp(mynodeid, node) && colo_is_slave())) {
            strcpy(client.master_id, node);
        } else {
            strcpy(client.slave_id, node);
        }
    }
    if (hb->llc_ops->end_nodewalk(hb) != HA_OK) {
        fprintf(stderr, "Cannot end node walk\n");
        goto err_signoff;
    }

    if (node_num != 2) {
        fprintf(stderr, "the num of node must be 2, not %d\n", node_num);
        goto err2;
    }

    client.deadtime = hb->llc_ops->get_deadtime(hb);
    client.keepalive = hb->llc_ops->get_keepalive(hb);
    client.master_alive =
        status_isalive(hb->llc_ops->node_status(hb, client.master_id));
    client.slave_alive =
        status_isalive(hb->llc_ops->node_status(hb, client.slave_id));
    if (!ignore && !client.master_alive) {
        fprintf(stderr, "slave or master is dead\n");
        goto err2;
    }

    client.hb = hb;
    client.status = CLIENT_RUNNING;

    return 0;
err_signoff:
    fprintf(stderr, "REASON: %s\n", hb->llc_ops->errmsg(hb));
err2:
    hb->llc_ops->signoff(hb, TRUE);
err:
    hb->llc_ops->delete(hb);

    return -1;
}

int register_heartbeat_client(void)
{
    if (heartbeat_client_init(0)) {
        return -1;
    }
    qemu_mutex_init(&client.mutex);
    client.thread = g_malloc0(sizeof(QemuThread));
    qemu_thread_create(client.thread, "colo heartbeat", heartbeat_thread,
                       NULL, QEMU_THREAD_JOINABLE);
    return 0;
}

void unregister_heartbeat_client(void)
{
    qemu_mutex_lock(&client.mutex);
    client.status = PARENT_EXIT;
    qemu_mutex_unlock(&client.mutex);
    if (client.thread && client.hb) {
        /* send msg to heartbeat thread, let it go on */
        const char *id = client.hb->llc_ops->get_mynodeid(client.hb);
        struct ha_msg *msg = ha_msg_new(0);
        ha_msg_add(msg, F_TYPE, "ping");
        client.hb->llc_ops->sendnodemsg(client.hb, msg, id);
        ha_msg_del(msg);
    }
}

/* bottom half */
void unregister_heartbeat_client_bh(void)
{
    if (client.thread) {
        qemu_thread_join(client.thread);
        g_free(client.thread);
        client.thread = NULL;
    }
}

#else /* #ifndef DISABLE_HEARTBEAT */

#include <migration/migration-failover.h>
bool vmstate_loading = false;
int get_heartbeat(bool slave) {return 1;}
void set_heartbeat(bool slave, int value) {}
int register_heartbeat_client(void) {return 0;}
void unregister_heartbeat_client(void) {}
void unregister_heartbeat_client_bh(void) {}
int heartbeat_deadtime(void) {return 1000;}

#endif /* #else #ifndef DISABLE_HEARTBEAT */
