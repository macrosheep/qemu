/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (C) 2014 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "migration/migration-colo.h"
#include "net/colo-nic.h"

bool colo_supported(void)
{
    return false;
}

void colo_init_checkpointer(MigrationState *s)
{
}

void *colo_process_incoming_checkpoints(void *opaque)
{
    return NULL;
}

bool colo_is_master(void)
{
    return false;
}

bool colo_is_slave(void)
{
    return false;
}

void colo_add_nic_devices(NetClientState *nc)
{
}

void colo_remove_nic_devices(NetClientState *nc)
{
}
