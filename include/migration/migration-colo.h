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

void colo_info_mig_init(void);

bool colo_supported(void);

/* save */
bool migrate_use_colo(void);
void colo_init_checkpointer(MigrationState *s);
bool is_master(void);

/* restore */
bool restore_use_colo(void);
void restore_exit_colo(void);
bool is_slave(void);

void colo_process_incoming_checkpoints(QEMUFile *f);

#endif
