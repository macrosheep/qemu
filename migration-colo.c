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
#include "migration/migration-colo.h"

static QEMUBH *colo_bh;

bool colo_supported(void)
{
    return true;
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

    colo_buffer_init();

    /*TODO: COLO checkpointed save loop*/

    colo_buffer_destroy();

    if (s->state != MIG_STATE_ERROR) {
        migrate_set_state(s, MIG_STATE_COLO, MIG_STATE_COMPLETED);
    }

    qemu_mutex_lock_iothread();
    qemu_bh_schedule(s->cleanup_bh);
    qemu_mutex_unlock_iothread();

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

void colo_process_incoming_checkpoints(QEMUFile *f)
{
    if (!restore_use_colo()) {
        return;
    }

    colo = qemu_coroutine_self();
    assert(colo != NULL);

    colo_buffer_init();

    /* TODO: COLO checkpointed restore loop */

    colo_buffer_destroy();
    colo = NULL;
    restore_exit_colo();

    return;
}
