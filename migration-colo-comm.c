/*
 *  COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 *  (a.k.a. Fault Tolerance or Continuous Replication)
 *
 *  Copyright (C) 2014 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */

#include <migration/migration-colo.h>

#define DEBUG_COLO

#ifdef DEBUG_COLO
#define DPRINTF(fmt, ...) \
    do { fprintf(stdout, "COLO: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

static bool colo_requested;

/* save */

bool migrate_use_colo(void)
{
    MigrationState *s = migrate_get_current();
    return s->enabled_capabilities[MIGRATION_CAPABILITY_COLO];
}

static void colo_info_save(QEMUFile *f, void *opaque)
{
    qemu_put_byte(f, migrate_use_colo());
}

/* restore */

static int colo_info_load(QEMUFile *f, void *opaque, int version_id)
{
    int value = qemu_get_byte(f);

    if (value && !colo_supported()) {
        fprintf(stderr, "COLO is not supported\n");
        return -EINVAL;
    }

    if (value && !colo_requested) {
        DPRINTF("COLO requested!\n");
    }

    colo_requested = value;

    return 0;
}

static SaveVMHandlers savevm_colo_info_handlers = {
    .save_state = colo_info_save,
    .load_state = colo_info_load,
};

void colo_info_mig_init(void)
{
    register_savevm_live(NULL, "colo info", -1, 1,
                         &savevm_colo_info_handlers, NULL);
}
