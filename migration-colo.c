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
#include "migration/migration-colo.h"
#include <sys/ioctl.h>

/*
 * checkpoint timer: unit ms
 * this is large because COLO checkpoint will mostly depend on
 * COLO compare module.
 */
#define CHKPOINT_TIMER 10000

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

static __attribute__((unused)) int colo_compare_flush(void)
{
    return ioctl(comp_fd, COMP_IOCTFLUSH, 1);
}

static __attribute__((unused)) int colo_compare_resume(void)
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

/* save */

static void *colo_thread(void *opaque)
{
    MigrationState *s = opaque;
    int dev_hotplug = qdev_hotplug, wait_cp = 0;
    int64_t start_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    int64_t current_time;

    if (colo_compare_init() < 0) {
        error_report("Init colo compare error\n");
        goto out;
    }

    qdev_hotplug = 0;

    colo_buffer_init();

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

        /*TODO: COLO save */

        start_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    }

out:
    colo_buffer_destroy();
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

/*
 * return:
 * 0: start a checkpoint
 * 1: some error happend, exit colo restore
 */
static int slave_wait_new_checkpoint(QEMUFile *f)
{
    /* TODO: wait checkpoint start command from master */
    return 1;
}

void colo_process_incoming_checkpoints(QEMUFile *f)
{
    int dev_hotplug = qdev_hotplug;

    if (!restore_use_colo()) {
        return;
    }

    qdev_hotplug = 0;

    colo = qemu_coroutine_self();
    assert(colo != NULL);

    colo_buffer_init();

    while (true) {
        if (slave_wait_new_checkpoint(f)) {
            break;
        }

        /* TODO: COLO restore */
    }

    colo_buffer_destroy();
    colo = NULL;
    restore_exit_colo();

    qdev_hotplug = dev_hotplug;

    return;
}
