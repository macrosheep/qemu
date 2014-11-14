/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (C) 2014 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#ifndef QEMU_MIGRATION_COLO_H
#define QEMU_MIGRATION_COLO_H

#include "qemu-common.h"
#include "migration/migration.h"
#include "block/coroutine.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"

void colo_info_mig_init(void);

bool colo_supported(void);

struct colo_incoming {
    QEMUFile *file;
    QemuThread thread;
    QEMUBH *bh;
};

enum {
    COLO_SIDE_MASTER = 0,
    COLO_SIDE_SLAVE,
};

/* save */
bool migrate_use_colo(void);
void colo_init_checkpointer(MigrationState *s);
bool colo_is_master(void);

/* restore */
extern Coroutine *migration_incoming_co;
bool restore_use_colo(void);
void restore_exit_colo(void);
void *colo_process_incoming_checkpoints(void *opaque);
bool colo_is_slave(void);

/* ram cache */
void create_and_init_ram_cache(void);
void release_ram_cache(void);

/* failover */
void colo_do_failover(MigrationState *s);

#endif
