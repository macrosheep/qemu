/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (C) 2014 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "hw/qdev-core.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "migration/migration-colo.h"
#include <sys/ioctl.h>
#include "qemu/error-report.h"
#include "migration/migration-failover.h"
#include "net/colo-nic.h"

/*
 * checkpoint timer: unit ms
 * this is large because COLO checkpoint will mostly depend on
 * COLO compare module.
 */
#define CHKPOINT_TIMER 10000

enum {
    COLO_READY = 0x46,

    /*
     * Checkpoint synchronzing points.
     *
     *                  Primary                 Secondary
     *  NEW             @
     *                                          Suspend
     *  SUSPENDED                               @
     *                  Suspend&Save state
     *  SEND            @
     *                  Send state              Receive state
     *  RECEIVED                                @
     *                  Flush network           Load state
     *  LOADED                                  @
     *                  Resume                  Resume
     *
     *                  Start Comparing
     * NOTE:
     * 1) '@' who sends the message
     * 2) Every sync-point is synchronized by two sides with only
     *    one handshake(single direction) for low-latency.
     *    If more strict synchronization is required, a opposite direction
     *    sync-point should be added.
     * 3) Since sync-points are single direction, the remote side may
     *    go forward a lot when this side just receives the sync-point.
     */
    COLO_CHECKPOINT_NEW,
    COLO_CHECKPOINT_SUSPENDED,
    COLO_CHECKPOINT_SEND,
    COLO_CHECKPOINT_RECEIVED,
    COLO_CHECKPOINT_LOADED,
};

static QEMUBH *colo_bh;
struct colo_incoming *colo_in = NULL;

bool colo_supported(void)
{
    return true;
}

/* colo buffer */
#define COLO_BUFFER_BASE_SIZE (1000*1000*4ULL)

QEMUSizedBuffer *colo_buffer = NULL;

/* colo agent */
#define COMP_IOC_MAGIC          'k'
#define COMP_IOCTWAIT           _IO(COMP_IOC_MAGIC, 0)
#define COMP_IOCTFLUSH          _IO(COMP_IOC_MAGIC, 1)
#define COMP_IOCTRESUME         _IO(COMP_IOC_MAGIC, 2)

#define COLO_IO                 0x33
#define COLO_CREATE_VM          _IO(COLO_IO, 0x00)
#define COLO_RELEASE_VM         _IO(COLO_IO, 0x01)

#define COMP_IOCTWAIT_TIMEOUT   5000

#define COMPARE_DEV "/dev/HA_compare"
/* COLO agent module FD */
static int agent_fd = -1;
static int vm_fd = -1;

static int colo_agent_init(void)
{
    agent_fd = open(COMPARE_DEV, O_RDWR);
    if (agent_fd < 0) {
        return -1;
    }

    vm_fd = ioctl(agent_fd, COLO_CREATE_VM, (int)getpid());
    if ( vm_fd < 0) {
        close(agent_fd);
        return -1;
    }

    return 0;
}

static void colo_agent_teardown(void)
{
    if (vm_fd >= 0) {
        close(vm_fd);
        vm_fd = -1;
        ioctl(agent_fd, COLO_RELEASE_VM, (int)getpid());
    }

    if (agent_fd >= 0) {
        close(agent_fd);
        agent_fd = -1;
    }
}

/*
 * Communicate with COLO Agent through ioctl.
 * return:
 * 0: start a checkpoint
 * other: errno == ETIME or ERESTART, try again
 *        errno == other, error, quit colo save
 */
static int colo_agent_wait_checkpoint(void)
{
    return ioctl(vm_fd, COMP_IOCTWAIT, COMP_IOCTWAIT_TIMEOUT);
}

static int colo_agent_preresume(void)
{
    return ioctl(vm_fd, COMP_IOCTFLUSH);
}

static int colo_agent_postresume(void)
{
    return ioctl(vm_fd, COMP_IOCTRESUME);
}

/* failover */
static void migration_co_bh(void *opaque)
{
    struct colo_incoming *colo_in = opaque;
    if (!colo_in->bh) {
        return ;
    }
    qemu_bh_delete(colo_in->bh);
    colo_in->bh = NULL;
    if (migration_incoming_co) {
        qemu_coroutine_enter(migration_incoming_co, NULL);
    }
}

static bool failover_completed = false;
void colo_do_failover(MigrationState *s)
{
    if (colo_is_slave()) {
        /* TODO: handle block cleanups */
        colo_teardown_nic(true);
    } else {
        /* TODO: handle block cleanups */
        colo_teardown_nic(false);
    }
    failover_completed = true;
}

/* colo checkpoint control helper */

static void ctl_error_handler(void *opaque, int err)
{
    if (colo_is_slave()) {
        /* determine whether we need to failover */
        if (!failover_request_is_set() && get_heartbeat(COLO_SIDE_MASTER)) {
            /* Wait for heartbeat deadtime */
            usleep((heartbeat_deadtime() + 100) * 1000);
            if (get_heartbeat(COLO_SIDE_MASTER)) {
                /*
                 * We assume that master is still alive according to heartbeat,
                 * just kill slave
                 */
                error_report("error: colo transmission failed!");
                colo_teardown_nic(true);
                exit(1);
            }
        }
        /*
         * OK, master dead, check failover request again, because failover
         * request may be set by heartbeat channel
         */
        if (!failover_request_is_set()) {
            error_report("failover request from checkpoint channel");
            failover_request_set();
            colo_do_failover(migrate_get_current());
            colo_in->bh = qemu_bh_new(migration_co_bh, colo_in);
            qemu_bh_schedule(colo_in->bh);
        }
    } else if (colo_is_master()) {
        /* Master still alive, do not failover */
        error_report("error: colo transmission failed!");
        error_report("master takeover from checkpoint channel");
        colo_do_failover(migrate_get_current());
        return;
    } else {
        error_report("COLO: Unexpected error happend!");
        exit(EXIT_FAILURE);
    }
}

static int colo_ctl_put(QEMUFile *f, uint64_t request)
{
    int ret = 0;

    qemu_put_be64(f, request);
    qemu_fflush(f);

    ret = qemu_file_get_error(f);
    if (ret < 0) {
        ctl_error_handler(f, ret);
        return 1;
    }

    return ret;
}

static int colo_ctl_get_value(QEMUFile *f, uint64_t *value)
{
    int ret = 0;
    uint64_t temp;

    temp = qemu_get_be64(f);

    ret = qemu_file_get_error(f);
    if (ret < 0) {
        ctl_error_handler(f, ret);
        return 1;
    }

    *value = temp;
    return 0;
}

static int colo_ctl_get(QEMUFile *f, uint64_t require)
{
    int ret;
    uint64_t value;

    ret = colo_ctl_get_value(f, &value);
    if (ret) {
        return ret;
    }

    if (value != require) {
        error_report("unexpected state! expected: %"PRIu64
                     ", received: %"PRIu64, require, value);
        exit(1);
    }

    return ret;
}

/* save */

bool colo_is_master(void)
{
    MigrationState *s = migrate_get_current();
    return (s->state == MIG_STATE_COLO);
}

static int do_colo_transaction(MigrationState *s, QEMUFile *control)
{
    int ret;
    size_t size;
    QEMUFile *trans = NULL;

    ret = colo_ctl_put(s->file, COLO_CHECKPOINT_NEW);
    if (ret) {
        goto out;
    }

    ret = colo_ctl_get(control, COLO_CHECKPOINT_SUSPENDED);
    if (ret) {
        goto out;
    }

    /* Reset colo buffer and open it for write */
    qsb_set_length(colo_buffer, 0);
    trans = qemu_bufopen("w", colo_buffer);
    if (!trans) {
        error_report("Open colo buffer for write failed");
        goto out;
    }

    if (failover_request_is_set()) {
        ret = -1;
        goto out;
    }
    /* suspend and save vm state to colo buffer */
    qemu_mutex_lock_iothread();
    vm_stop_force_state(RUN_STATE_COLO);
    qemu_mutex_unlock_iothread();
    /*
     * heartbeat failover request bh could be called during
     * vm_stop_force_state so we check failover_request_is_set() again.
     */
    if (failover_request_is_set()) {
        ret = -1;
        goto out;
    }
    /* Disable block migration */
    s->params.blk = 0;
    s->params.shared = 0;
    qemu_mutex_lock_iothread();
    qemu_savevm_state_begin(trans, &s->params);
    qemu_savevm_state_complete(trans);
    qemu_mutex_unlock_iothread();

    qemu_fflush(trans);

    ret = colo_ctl_put(s->file, COLO_CHECKPOINT_SEND);
    if (ret) {
        goto out;
    }

    /* send vmstate to slave */

    /* we send the total size of the vmstate first */
    size = qsb_get_length(colo_buffer);
    ret = colo_ctl_put(s->file, size);
    if (ret) {
        goto out;
    }

    qsb_put_buffer(s->file, colo_buffer, size);
    qemu_fflush(s->file);
    ret = qemu_file_get_error(s->file);
    if (ret < 0) {
        goto out;
    }

    ret = colo_ctl_get(control, COLO_CHECKPOINT_RECEIVED);
    if (ret) {
        goto out;
    }

    ret = colo_ctl_get(control, COLO_CHECKPOINT_LOADED);
    if (ret) {
        goto out;
    }

    ret = 0;

out:
    if (trans)
        qemu_fclose(trans);

    /* Flush network etc. */
    colo_agent_preresume();

    /* resume master */
    qemu_mutex_lock_iothread();
    vm_start();
    qemu_mutex_unlock_iothread();

    colo_agent_postresume();

    return ret;
}

static void *colo_thread(void *opaque)
{
    MigrationState *s = opaque;
    int dev_hotplug = qdev_hotplug, wait_cp = 0;
    int64_t start_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    int64_t current_time;
    QEMUFile *colo_control = NULL;
    int ret;

    if (register_heartbeat_client()) {
        error_report("register heartbeat failed\n");
        goto out;
    }

    colo_configure_nic(false);

    if (colo_agent_init() < 0) {
        error_report("Init colo agent error");
        goto out;
    }

    colo_control = qemu_fopen_socket(qemu_get_fd(s->file), "rb");
    if (!colo_control) {
        error_report("Open colo_control failed!");
        goto out;
    }

    qdev_hotplug = 0;

    /*
     * Wait for slave finish loading vm states and enter COLO
     * restore.
     */
    ret = colo_ctl_get(colo_control, COLO_READY);
    if (ret) {
        goto out;
    }

    colo_buffer = qsb_create(NULL, COLO_BUFFER_BASE_SIZE);

    while (s->state == MIG_STATE_COLO) {
        /* wait for a colo checkpoint */
        wait_cp = colo_agent_wait_checkpoint();
        if (wait_cp) {
            if (errno != ETIME && errno != ERESTART) {
                error_report("COLO agent module failed with errno(%s)", strerror(errno));
                goto out;
            }
            /*
             * No checkpoint is needed, wait for 1ms and then
             * check if we need checkpoint again
             */
            current_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
            if (current_time - start_time < CHKPOINT_TIMER) {
                usleep(1000);
                continue;
            }
        }

        /* start a colo checkpoint */
        if (do_colo_transaction(s, colo_control)) {
            goto out;
        }

        start_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    }

out:
    /* if we went here, means slave may dead, we are taking over */
    if (failover_request_is_set()) {
        while (!failover_completed) {
            ;
        }
    }

    if (colo_buffer) {
        qsb_free(colo_buffer);
    }

    if (colo_control) {
        qemu_fclose(colo_control);
    }

    unregister_heartbeat_client();
    colo_agent_teardown();
    colo_teardown_nic(false);

    migrate_set_state(s, MIG_STATE_COLO, MIG_STATE_COMPLETED);

    qemu_mutex_lock_iothread();
    qemu_bh_schedule(s->cleanup_bh);
    qemu_mutex_unlock_iothread();

    qdev_hotplug = dev_hotplug;

    return NULL;
}

static void colo_start_checkpointer(void *opaque)
{
    MigrationState *s = opaque;

    if (colo_bh) {
        qemu_bh_delete(colo_bh);
        colo_bh = NULL;
    }

    qemu_mutex_unlock_iothread();
    qemu_thread_join(&s->thread);
    qemu_mutex_lock_iothread();

    migrate_set_state(s, MIG_STATE_ACTIVE, MIG_STATE_COLO);

    qemu_thread_create(&s->thread, "colo", colo_thread, s,
                       QEMU_THREAD_JOINABLE);
}

void colo_init_checkpointer(MigrationState *s)
{
    colo_bh = qemu_bh_new(colo_start_checkpointer, s);
    qemu_bh_schedule(colo_bh);
}

/* restore */

Coroutine *colo = NULL;

bool colo_is_slave(void)
{
    return colo != NULL;
}

/*
 * return:
 * 0: start a checkpoint
 * 1: some error happend, exit colo restore
 */
static int slave_wait_new_checkpoint(QEMUFile *f)
{
    int ret;
    uint64_t cmd;

    ret = colo_ctl_get_value(f, &cmd);
    if (ret) {
        return 1;
    }

    if (cmd == COLO_CHECKPOINT_NEW) {
        return 0;
    } else {
        /* Unexpected data received */
        ctl_error_handler(f, ret);
        return 1;
    }
}

void *colo_process_incoming_checkpoints(void *opaque)
{
    colo_in = opaque;
    QEMUFile *f = colo_in->file;
    int fd = qemu_get_fd(f);
    int dev_hotplug = qdev_hotplug;
    QEMUFile *ctl = NULL, *fb = NULL;
    int ret;
    uint64_t total_size;

    if (!restore_use_colo()) {
        return NULL;
    }

    qdev_hotplug = 0;

    colo = qemu_coroutine_self();
    assert(colo != NULL);

    ctl = qemu_fopen_socket(fd, "wb");
    if (!ctl) {
        error_report("Can't open incoming channel!");
        goto out;
    }

    create_and_init_ram_cache();
    colo_configure_nic(true);

    if (register_heartbeat_client()) {
        error_report("register heartbeat failed\n");
        goto out;
    }

    ret = colo_ctl_put(ctl, COLO_READY);
    if (ret) {
        goto out;
    }

    colo_buffer = qsb_create(NULL, COLO_BUFFER_BASE_SIZE);
    if (colo_buffer == NULL) {
        error_report("Failed to allocate colo buffer!");
        goto out;
    }

    /* in COLO mode, slave is runing, so start the vm */
    vm_start();

    while (true) {
        if (slave_wait_new_checkpoint(f)) {
            break;
        }

        if (failover_request_is_set()) {
            error_report("failover request from heartbeat channel");
            goto out;
        }

        /* start colo checkpoint */

        /* suspend guest */
        qemu_mutex_lock_iothread();
        vm_stop_force_state(RUN_STATE_COLO);
        qemu_mutex_unlock_iothread();

        ret = colo_ctl_put(ctl, COLO_CHECKPOINT_SUSPENDED);
        if (ret) {
            goto out;
        }

        ret = colo_ctl_get(f, COLO_CHECKPOINT_SEND);
        if (ret) {
            goto out;
        }

        /* read migration data into colo buffer */

        /* read the vmstate total size first */
        ret = colo_ctl_get_value(f, &total_size);
        if (ret) {
            goto out;
        }
        ret = qsb_fill_buffer(colo_buffer, f, total_size);
        if (ret != total_size) {
            error_report("can't get all migration data");
            goto out;
        }

        ret = colo_ctl_put(ctl, COLO_CHECKPOINT_RECEIVED);
        if (ret) {
            goto out;
        }

        /* open colo buffer for read */
        fb = qemu_bufopen("r", colo_buffer);
        if (!fb) {
            error_report("can't open colo buffer for read");
            goto out;
        }

        /* load vm state */
        qemu_mutex_lock_iothread();
        if (qemu_loadvm_state(fb) < 0) {
            error_report("COLO: loadvm failed\n");
            qemu_mutex_unlock_iothread();
            goto out;
        }
        qemu_mutex_unlock_iothread();

        ret = colo_ctl_put(ctl, COLO_CHECKPOINT_LOADED);
        if (ret) {
            goto out;
        }

        /* resume guest */
        qemu_mutex_lock_iothread();
        vm_start();
        qemu_mutex_unlock_iothread();

        qemu_fclose(fb);
        fb = NULL;
    }

out:
    /* if we went here, means master may dead, we are taking over */
    if (failover_request_is_set()) {
        while (!failover_completed) {
            ;
        }
    }
    colo = NULL;

    if (fb) {
        qemu_fclose(fb);
    }

    release_ram_cache();
    unregister_heartbeat_client();
    colo_teardown_nic(true);

    if (ctl) {
        qemu_fclose(ctl);
    }

    if (colo_buffer) {
        qsb_free(colo_buffer);
    }

    restore_exit_colo();

    qdev_hotplug = dev_hotplug;

    return NULL;
}
