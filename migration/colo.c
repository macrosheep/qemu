/*
 *  COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 *  (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (C) 2015 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "block/coroutine.h"
#include "migration/migration-colo.h"

static QEMUBH *colo_bh;

bool colo_supported(void)
{
    return true;
}

/* save */

static void *colo_thread(void *opaque)
{
    MigrationState *s = opaque;

    /*TODO: COLO checkpointed save loop*/

    migrate_set_state(s, MIG_STATE_COLO, MIG_STATE_COMPLETED);

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

static Coroutine *colo = NULL;

void *colo_process_incoming_checkpoints(void *opaque)
{
    if (!restore_use_colo()) {
        return NULL;
    }

    colo = qemu_coroutine_self();
    assert(colo != NULL);

    /* TODO: COLO checkpointed restore loop */

    colo = NULL;
    restore_exit_colo();

    return NULL;
}
