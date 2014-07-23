/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (C) 2014 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "block/coroutine.h"
#include "hw/qdev-core.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "migration/migration-colo.h"
#include <sys/ioctl.h>
#include "qemu/error-report.h"

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

bool colo_supported(void)
{
    return true;
}

/* colo compare */
#define COMP_IOC_MAGIC 'k'
#define COMP_IOCTWAIT   _IO(COMP_IOC_MAGIC, 0)
#define COMP_IOCTFLUSH  _IO(COMP_IOC_MAGIC, 1)
#define COMP_IOCTRESUME _IO(COMP_IOC_MAGIC, 2)

#define COMPARE_DEV "/dev/HA_compare"
/* COLO compare module FD */
static int comp_fd = -1;

static int colo_compare_init(void)
{
    comp_fd = open(COMPARE_DEV, O_RDONLY);
    if (comp_fd < 0) {
        return -1;
    }

    return 0;
}

static void colo_compare_destroy(void)
{
    if (comp_fd >= 0) {
        close(comp_fd);
        comp_fd = -1;
    }
}

/*
 * Communicate with COLO Agent through ioctl.
 * return:
 * 0: start a checkpoint
 * other: errno == ETIME or ERESTART, try again
 *        errno == other, error, quit colo save
 */
static int colo_compare(void)
{
    return ioctl(comp_fd, COMP_IOCTWAIT, 250);
}

static int colo_compare_flush(void)
{
    return ioctl(comp_fd, COMP_IOCTFLUSH, 1);
}

static int colo_compare_resume(void)
{
    return ioctl(comp_fd, COMP_IOCTRESUME, 1);
}

/* colo checkpoint control helper */

static void ctl_error_handler(void *opaque, int err)
{
    if (colo_is_slave()) {
        /* TODO: determine whether we need to failover */
        /* FIXME: we will not failover currently, just kill slave */
        error_report("error: colo transmission failed!");
        exit(1);
    } else if (colo_is_master()) {
        /* Master still alive, do not failover */
        error_report("error: colo transmission failed!");
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
    uint8_t *buf;
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

    /* open colo buffer for write */
    trans = qemu_bufopen("w", NULL);
    if (!trans) {
        error_report("Open colo buffer for write failed");
        goto out;
    }

    /* suspend and save vm state to colo buffer */
    qemu_mutex_lock_iothread();
    vm_stop_force_state(RUN_STATE_COLO);
    qemu_mutex_unlock_iothread();
    /* Disable block migration */
    s->params.blk = 0;
    s->params.shared = 0;
    qemu_savevm_state_begin(trans, &s->params);
    qemu_savevm_state_complete(trans);

    qemu_fflush(trans);

    ret = colo_ctl_put(s->file, COLO_CHECKPOINT_SEND);
    if (ret) {
        goto out;
    }

    /* send vmstate to slave */

    /* we send the total size of the vmstate first */
    size = qsb_get_length(qemu_buf_get(trans));
    ret = colo_ctl_put(s->file, size);
    if (ret) {
        goto out;
    }

    buf = g_malloc(size);
    qsb_get_buffer(qemu_buf_get(trans), 0, size, buf);
    qemu_put_buffer(s->file, buf, size);
    g_free(buf);
    ret = qemu_file_get_error(s->file);
    if (ret < 0) {
        goto out;
    }
    qemu_fflush(s->file);

    ret = colo_ctl_get(control, COLO_CHECKPOINT_RECEIVED);
    if (ret) {
        goto out;
    }

    /* Flush network etc. */
    colo_compare_flush();

    ret = colo_ctl_get(control, COLO_CHECKPOINT_LOADED);
    if (ret) {
        goto out;
    }

    colo_compare_resume();
    ret = 0;

out:
    if (trans)
        qemu_fclose(trans);
    /* resume master */
    qemu_mutex_lock_iothread();
    vm_start();
    qemu_mutex_unlock_iothread();

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

    if (colo_compare_init() < 0) {
        error_report("Init colo compare error");
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

    while (s->state == MIG_STATE_COLO) {
        /* wait for a colo checkpoint */
        wait_cp = colo_compare();
        if (wait_cp) {
            if (errno != ETIME && errno != ERESTART) {
                error_report("compare module failed(%s)", strerror(errno));
                goto out;
            }
            /*
             * no checkpoint is needed, wait for 1ms and then
             * check if we need checkpoint
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
    if (colo_control) {
        qemu_fclose(colo_control);
    }

    colo_compare_destroy();

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

static Coroutine *colo;

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
    int fd = qemu_get_fd(f);
    int ret;
    uint64_t cmd;

    yield_until_fd_readable(fd);

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

void colo_process_incoming_checkpoints(QEMUFile *f)
{
    int fd = qemu_get_fd(f);
    int dev_hotplug = qdev_hotplug;
    QEMUFile *ctl = NULL, *fb = NULL;
    int ret;
    uint64_t total_size;
    uint8_t *buf;
    QEMUSizedBuffer *colo_buffer;

    if (!restore_use_colo()) {
        return;
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

    ret = colo_ctl_put(ctl, COLO_READY);
    if (ret) {
        goto out;
    }

    /* in COLO mode, slave is runing, so start the vm */
    vm_start();

    while (true) {
        if (slave_wait_new_checkpoint(f)) {
            break;
        }

        /* start colo checkpoint */

        /* suspend guest */
        vm_stop_force_state(RUN_STATE_COLO);

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
        buf = g_malloc(total_size);
        qemu_get_buffer(f, buf, total_size);
        colo_buffer = qsb_create(buf, total_size);
        g_free(buf);

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
        if (qemu_loadvm_state(fb) < 0) {
            error_report("COLO: loadvm failed\n");
            goto out;
        }

        ret = colo_ctl_put(ctl, COLO_CHECKPOINT_LOADED);
        if (ret) {
            goto out;
        }

        /* resume guest */
        vm_start();

        qemu_fclose(fb);
        fb = NULL;
    }

out:
    colo = NULL;

    if (fb) {
        qemu_fclose(fb);
    }

    release_ram_cache();

    if (ctl) {
        qemu_fclose(ctl);
    }

    restore_exit_colo();

    qdev_hotplug = dev_hotplug;

    return;
}
