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
#include "migration/migration-colo.h"
#include <sys/ioctl.h>
#include "qemu/error-report.h"

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

/* colo agent */
#define COMP_IOC_MAGIC          'k'
#define COMP_IOCTWAIT           _IO(COMP_IOC_MAGIC, 0)
#define COMP_IOCTFLUSH          _IO(COMP_IOC_MAGIC, 1)
#define COMP_IOCTRESUME         _IO(COMP_IOC_MAGIC, 2)

#define COLO_IO                 0x33
#define COLO_CREATE_VM          _IO(COLO_IO, 0x00)
#define COLO_RELEASE_VM         _IO(COLO_IO, 0x01)

#define COMP_IOCTWAIT_TIMEOUT   5000

#define COMPARE_DEV "/dev/HA_compare"
/* COLO agent module FD */
static int agent_fd = -1;
static int vm_fd = -1;

static int colo_agent_init(void)
{
    agent_fd = open(COMPARE_DEV, O_RDWR);
    if (agent_fd < 0) {
        return -1;
    }

    vm_fd = ioctl(agent_fd, COLO_CREATE_VM, (int)getpid());
    if ( vm_fd < 0) {
        close(agent_fd);
        return -1;
    }

    return 0;
}

static void colo_agent_teardown(void)
{
    if (vm_fd >= 0) {
        close(vm_fd);
        vm_fd = -1;
        ioctl(agent_fd, COLO_RELEASE_VM, (int)getpid());
    }

    if (agent_fd >= 0) {
        close(agent_fd);
        agent_fd = -1;
    }
}

/*
 * Communicate with COLO Agent through ioctl.
 * return:
 * 0: start a checkpoint
 * other: errno == ETIME or ERESTART, try again
 *        errno == other, error, quit colo save
 */
static int colo_agent_wait_checkpoint(void)
{
    return ioctl(vm_fd, COMP_IOCTWAIT, COMP_IOCTWAIT_TIMEOUT);
}

static __attribute__((unused)) int colo_agent_preresume(void)
{
    return ioctl(vm_fd, COMP_IOCTFLUSH);
}

static __attribute__((unused)) int colo_agent_postresume(void)
{
    return ioctl(vm_fd, COMP_IOCTRESUME);
}

/* save */

static __attribute__((unused)) bool colo_is_master(void)
{
    MigrationState *s = migrate_get_current();
    return (s->state == MIG_STATE_COLO);
}

static void *colo_thread(void *opaque)
{
    MigrationState *s = opaque;
    int dev_hotplug = qdev_hotplug, wait_cp = 0;
    int64_t start_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    int64_t current_time;

    if (colo_agent_init() < 0) {
        error_report("Init colo agent error");
        goto out;
    }

    qdev_hotplug = 0;

    while (s->state == MIG_STATE_COLO) {
        /* wait for a colo checkpoint */
        wait_cp = colo_agent_wait_checkpoint();
        if (wait_cp) {
            if (errno != ETIME && errno != ERESTART) {
                error_report("COLO agent module failed with errno(%s)", strerror(errno));
                goto out;
            }
            /*
             * No checkpoint is needed, wait for 1ms and then
             * check if we need checkpoint again
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
    colo_agent_teardown();

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

static __attribute__((unused)) bool colo_is_slave(void)
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

    while (true) {
        if (slave_wait_new_checkpoint(f)) {
            break;
        }

        /* TODO: COLO restore */
    }

    colo = NULL;
    restore_exit_colo();

    qdev_hotplug = dev_hotplug;

    return;
}
