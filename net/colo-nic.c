/*
 *  COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 *  (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (C) 2014 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */
#include "net/net.h"
#include "net/colo-nic.h"
#include "qemu/error-report.h"
#include "migration/migration-colo.h"

typedef struct nic_device {
    NetClientState *nc;
    bool (*support_colo)(NetClientState *nc);
    int (*configure)(NetClientState *nc, bool up, bool is_slave);
    QTAILQ_ENTRY(nic_device) next;
    bool is_up;
} nic_device;

QTAILQ_HEAD(, nic_device) nic_devices = QTAILQ_HEAD_INITIALIZER(nic_devices);

static bool nic_support_colo(NetClientState *nc)
{
    return nc && nc->colo_script[0] && nc->colo_nicname[0];
}

#define STDOUT_BUF_LEN 1024
static char stdout_buf[STDOUT_BUF_LEN];

static int launch_colo_script(char *argv[])
{
    int pid, status;
    char *script = argv[0];
    int fds[2];

    bzero(stdout_buf, sizeof(stdout_buf));

    if (pipe(fds) < 0) {
        return -1;
    }
    /* try to launch network script */
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        execv(script, argv);
        _exit(1);
    } else if (pid > 0) {
        FILE *stream;
        int n;
        close(fds[1]);
        stream = fdopen(fds[0], "r");
        n = fread(stdout_buf, 1, STDOUT_BUF_LEN - 1, stream);
        stdout_buf[n] = '\0';
        close(fds[0]);

        while (waitpid(pid, &status, 0) != pid) {
            /* loop */
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0;
        }
    }
    fprintf(stderr, "%s\n", stdout_buf);
    fprintf(stderr, "%s: could not launch network script\n", script);
    return -1;
}

static void store_ifbname(NetClientState *nc)
{
    char *str_b = NULL, *str_e = NULL;

    str_b = strstr(stdout_buf, "ifb0=");
    if (str_b) {
        str_e = strstr(str_b, "\n");
    }
    if (str_e) {
        snprintf(nc->ifb[0], str_e - str_b - 5 + 1, "%s", str_b + 5);
    }

    str_b = str_e = NULL;
    str_b = strstr(stdout_buf, "ifb1=");
    if (str_b) {
        str_e = strstr(str_b, "\n");
    }
    if (str_e) {
        snprintf(nc->ifb[1], str_e - str_b - 5 + 1, "%s", str_b + 5);
    }
}

static int nic_configure(NetClientState *nc, bool up, bool is_slave)
{
    char *argv[8];
    char **parg;
    int ret = -1, i;
    int argc = (!is_slave && !up) ? 7 : 5;

    if (!nc) {
        error_report("Can not parse colo_script or colo_nicname");
        return ret;
    }

    parg = argv;
    *parg++ = nc->colo_script;
    *parg++ = (char *)(is_slave ? "slaver" : "master");
    *parg++ = (char *)(up ? "install" : "uninstall");
    *parg++ = nc->ifname;
    *parg++ = nc->colo_nicname;
    if (!is_slave && !up) {
        *parg++ = nc->ifb[0];
        *parg++ = nc->ifb[1];
    }
    *parg = NULL;

    for (i = 0; i < argc; i++) {
        if (!argv[i][0]) {
            error_report("Can not get colo_script argument");
            return ret;
        }
    }

    ret = launch_colo_script(argv);
    if (!is_slave && up && ret == 0) {
        store_ifbname(nc);
    }

    return ret;
}

static int configure_one_nic(NetClientState *nc, bool up, bool is_slave)
{
    struct nic_device *nic;

    if (!nc) {
        return -1;
    }

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        if (nic->nc == nc) {
            if (!nic->support_colo || !nic->support_colo(nic->nc)
                || !nic->configure) {
                return -1;
            }
            if (up == nic->is_up) {
                return 0;
            }

            if (nic->configure(nic->nc, up, is_slave) && up) {
                return -1;
            }
            nic->is_up = up;
            return 0;
        }
    }

    return -1;
}

void colo_add_nic_devices(NetClientState *nc)
{
    struct nic_device *nic = g_malloc0(sizeof(*nic));

    nic->support_colo = nic_support_colo;
    nic->configure = nic_configure;

    /*
     * TODO
     * only support "-netdev tap,colo_scripte..."  options
     * "-net nic -net tap..." options is not supported
     */
    nic->nc = nc;

    QTAILQ_INSERT_TAIL(&nic_devices, nic, next);
}

void colo_remove_nic_devices(NetClientState *nc)
{
    struct nic_device *nic, *next_nic;

    if (!nc) {
        return;
    }

    QTAILQ_FOREACH_SAFE(nic, &nic_devices, next, next_nic) {
        if (nic->nc == nc) {
            if (colo_is_slave()) {
                configure_one_nic(nc, 0, 1);
            }
            if (colo_is_master()) {
                configure_one_nic(nc, 0, 0);
            }
            QTAILQ_REMOVE(&nic_devices, nic, next);
            g_free(nic);
        }
    }
}

int colo_configure_nic(bool is_slave)
{
    struct nic_device *nic;

    if (QTAILQ_EMPTY(&nic_devices)) {
        return -1;
    }

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        if (configure_one_nic(nic->nc, 1, is_slave)) {
            return -1;
        }
    }

    return 0;
}

void colo_teardown_nic(bool is_slave)
{
    struct nic_device *nic;

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        configure_one_nic(nic->nc, 0, is_slave);
    }
}
