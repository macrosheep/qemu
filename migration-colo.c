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
#include "qemu/error-report.h"
#include "hw/qdev-core.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "migration/migration-colo.h"
#include <sys/ioctl.h>

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

/* colo buffer */

#define COLO_BUFFER_BASE_SIZE (1000*1000*4ULL)
#define COLO_BUFFER_MAX_SIZE (1000*1000*1000*10ULL)

typedef struct colo_buffer {
    uint8_t *data;
    uint64_t used;
    uint64_t freed;
    uint64_t size;
} colo_buffer_t;

static colo_buffer_t colo_buffer;

static void colo_buffer_init(void)
{
    if (colo_buffer.size == 0) {
        colo_buffer.data = g_malloc(COLO_BUFFER_BASE_SIZE);
        colo_buffer.size = COLO_BUFFER_BASE_SIZE;
    }
    colo_buffer.used = 0;
    colo_buffer.freed = 0;
}

static void colo_buffer_destroy(void)
{
    if (colo_buffer.data) {
        g_free(colo_buffer.data);
        colo_buffer.data = NULL;
    }
    colo_buffer.used = 0;
    colo_buffer.freed = 0;
    colo_buffer.size = 0;
}

static void colo_buffer_extend(uint64_t len)
{
    if (len > colo_buffer.size - colo_buffer.used) {
        len = len + colo_buffer.used - colo_buffer.size;
        len = ROUND_UP(len, COLO_BUFFER_BASE_SIZE) + COLO_BUFFER_BASE_SIZE;

        colo_buffer.size += len;
        if (colo_buffer.size > COLO_BUFFER_MAX_SIZE) {
            error_report("colo_buffer overflow!\n");
            exit(EXIT_FAILURE);
        }
        colo_buffer.data = g_realloc(colo_buffer.data, colo_buffer.size);
    }
}

static int colo_put_buffer(void *opaque, const uint8_t *buf,
                           int64_t pos, int size)
{
    colo_buffer_extend(size);
    memcpy(colo_buffer.data + colo_buffer.used, buf, size);
    colo_buffer.used += size;

    return size;
}

static int colo_get_buffer_internal(uint8_t *buf, int size)
{
    if ((size + colo_buffer.freed) > colo_buffer.used) {
        size = colo_buffer.used - colo_buffer.freed;
    }
    memcpy(buf, colo_buffer.data + colo_buffer.freed, size);
    colo_buffer.freed += size;

    return size;
}

static int colo_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    return colo_get_buffer_internal(buf, size);
}

static int colo_close(void *opaque)
{
    colo_buffer_t *cb = opaque ;

    cb->used = 0;
    cb->freed = 0;

    return 0;
}

static int colo_get_fd(void *opaque)
{
    /* colo buffer, no fd */
    return -1;
}

static const QEMUFileOps colo_write_ops = {
    .put_buffer = colo_put_buffer,
    .get_fd = colo_get_fd,
    .close = colo_close,
};

static const QEMUFileOps colo_read_ops = {
    .get_buffer = colo_get_buffer,
    .get_fd = colo_get_fd,
    .close = colo_close,
};

/* colo checkpoint control helper */

static void ctl_error_handler(void *opaque, int err)
{
    if (is_slave()) {
        /* TODO: determine whether we need to failover */
        /* FIXME: we will not failover currently, just kill slave */
        error_report("error: colo transmission failed!\n");
        exit(1);
    } else if (is_master()) {
        /* Master still alive, do not failover */
        error_report("error: colo transmission failed!\n");
        return;
    } else {
        error_report("COLO: Unexpected error happend!\n");
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
        error_report("unexpected state received!\n");
        exit(1);
    }

    return ret;
}

/* save */

bool is_master(void)
{
    MigrationState *s = migrate_get_current();
    return (s->state == MIG_STATE_COLO);
}

static int do_colo_transaction(MigrationState *s, QEMUFile *control,
                               QEMUFile *trans)
{
    int ret;

    ret = colo_ctl_put(s->file, COLO_CHECKPOINT_NEW);
    if (ret) {
        goto out;
    }

    ret = colo_ctl_get(control, COLO_CHECKPOINT_SUSPENDED);
    if (ret) {
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
    ret = colo_ctl_put(s->file, colo_buffer.used);
    if (ret) {
        goto out;
    }

    qemu_put_buffer_async(s->file, colo_buffer.data, colo_buffer.used);
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
    QEMUFile *colo_control = NULL, *colo_trans = NULL;
    int ret;

    if (colo_compare_init() < 0) {
        error_report("Init colo compare error\n");
        goto out;
    }

    colo_control = qemu_fopen_socket(qemu_get_fd(s->file), "rb");
    if (!colo_control) {
        error_report("open colo_control failed\n");
        goto out;
    }

    qdev_hotplug = 0;

    colo_buffer_init();

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

        /* open colo buffer for write */
        colo_trans = qemu_fopen_ops(&colo_buffer, &colo_write_ops);
        if (!colo_trans) {
            error_report("open colo buffer failed\n");
            goto out;
        }

        if (do_colo_transaction(s, colo_control, colo_trans)) {
            goto out;
        }

        qemu_fclose(colo_trans);
        colo_trans = NULL;
        start_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    }

out:
    if (colo_trans) {
        qemu_fclose(colo_trans);
    }

    colo_buffer_destroy();

    if (colo_control) {
        qemu_fclose(colo_control);
    }

    colo_compare_destroy();

    if (s->state != MIG_STATE_ERROR) {
        migrate_set_state(s, MIG_STATE_COLO, MIG_STATE_COMPLETED);
    }

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

bool is_slave(void)
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

    if (!restore_use_colo()) {
        return;
    }

    qdev_hotplug = 0;

    colo = qemu_coroutine_self();
    assert(colo != NULL);

    ctl = qemu_fopen_socket(fd, "wb");
    if (!ctl) {
        error_report("can't open incoming channel\n");
        goto out;
    }

    colo_buffer_init();

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

        /* open colo buffer for read */
        fb = qemu_fopen_ops(&colo_buffer, &colo_read_ops);
        if (!fb) {
            error_report("can't open colo buffer\n");
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
        colo_buffer_extend(total_size);
        qemu_get_buffer(f, colo_buffer.data, total_size);
        colo_buffer.used = total_size;

        ret = colo_ctl_put(ctl, COLO_CHECKPOINT_RECEIVED);
        if (ret) {
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
    colo_buffer_destroy();
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
