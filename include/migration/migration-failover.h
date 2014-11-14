/*
 *  COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 *  (a.k.a. Fault Tolerance or Continuous Replication)
 *
 *  Copyright Fujitsu, Corp. 2014
 *
 *  Authors:
 *   Yang Hongyang <yanghy@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */

#ifndef MIGRATION_FAILOVER_H
#define MIGRATION_FAILOVER_H

#include "qemu-common.h"

enum {
    CLIENT_RUNNING = 0xaa,
    PARENT_EXIT,
    CLIENT_RESTART,
};

extern bool vmstate_loading;

void failover_request_set(void);
void failover_request_clear(void);
bool failover_request_is_set(void);

int get_heartbeat(bool slave);
void set_heartbeat(bool slave, int value);
int register_heartbeat_client(void);
void unregister_heartbeat_client(void);
void unregister_heartbeat_client_bh(void);
int heartbeat_deadtime(void);

#endif
