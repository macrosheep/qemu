/*
 *  COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 *  (a.k.a. Fault Tolerance or Continuous Replication)
 *
 *  Copyright (C) 2014 FUJITSU LIMITED
 *
 *  Authors:
 *   Yang Hongyang <yanghy@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */

#include "migration/migration-failover.h"

static bool failover_request = false;

void failover_request_set(void)
{
    failover_request = true;
}

void failover_request_clear(void)
{
    failover_request = false;
}

bool failover_request_is_set(void)
{
    return failover_request;
}
