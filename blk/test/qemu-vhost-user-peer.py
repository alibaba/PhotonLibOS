#!/usr/bin/env python3
"""Point QEMU's own vhost-user-blk qtest at photon's backend instead of QSD.

QEMU's tests/qtest/vhost-user-blk-test.c is the only readily available
INDEPENDENT vhost-user frontend: it runs qemu-system's real vhost-user frontend
and drives it with libqos's user-space virtio-blk driver under -accel qtest, so
it needs no guest kernel, no TCG and no /dev/kvm. Its usual peer is
qemu-storage-daemon, which means the six cases it already has
(basic, indirect, idx, nxvirtq, hotplug, multiqueue) are an A/B baseline for us:
same frontend, same driver, only the backend swapped.

Why this is a script and not a patch file: it has to be applied to a QEMU tree
that is not this repository's, at whatever version that tree happens to be, so
anchored replacement that fails loudly beats a diff that applies fuzzily or not
at all. Every anchor must match exactly once or nothing is written.

    ./qemu-vhost-user-peer.py /path/to/qemu/tests/qtest/vhost-user-blk-test.c

Idempotent: a file that already carries the photon peer is left alone. The
patched test selects the peer by environment, so the QSD path still works:

    PHOTON_VHU_BACKEND=/path/to/vhost-user-cli   # photon backend
    QTEST_QEMU_STORAGE_DAEMON_BINARY=...         # required either way: the test
                                                 # registers nothing without it

Do NOT also set PHOTON_VHU_QUEUES: the patched test derives it from each case's
own num_queues and overwrites whatever is in the environment, so a value you
export proves nothing about that forwarding -- and hiding a regression in it is
exactly how this script once shipped a peer that always served one queue.
"""

import sys

INCLUDE_ANCHOR = '#include "libqos/libqos-pc.h"\n'
INCLUDES = '#include <sys/socket.h>\n#include <sys/un.h>\n'

FUNC_ANCHOR = ('static void start_vhost_user_blk(GString *cmd_line, '
               'int vus_instances,\n'
               '                                 int num_queues)\n'
               '{\n')

FUNC = r'''/*
 * photon's blk/vhost-user backend as the peer, selected by PHOTON_VHU_BACKEND.
 *
 * It cannot stand in for qemu-storage-daemon verbatim: QSD accepts on a listening
 * fd inherited from this test (addr.type=fd), whereas photon's backend binds and
 * listens itself -- its SERVER role, and the deployment shape its blk.h
 * documents. So this variant hands QEMU a path photon owns, and polls until
 * photon is listening: it needs photon::init() before it binds, and the chardev
 * below has no reconnect=, so a QEMU that started first would fail device realize.
 *
 * The connect probe does open one throwaway session on the backend, which sees
 * EOF and goes back to accepting -- worth knowing when reading its log.
 *
 * The peer's output goes to a file rather than /dev/null: when a vhost-user
 * backend and QEMU disagree about the protocol, that log is the only evidence.
 *
 * num_queues reaches the peer as PHOTON_VHU_QUEUES rather than as an argument,
 * because the peer's argv slots are already the socket, the image and the image
 * size -- see blk/vhost-user-cli.cc, which documents the variable. It does
 * have to reach it: vhost_user_backend_init() asks a backend how many queues it
 * serves with GET_QUEUE_NUM and rejects any device asking for more, so a peer
 * left at its one-queue default makes device_add num-queues=8 fail with "The
 * maximum number of queues supported by the backend is 1", and libqtest asserts
 * on any QMP error -- a red that reads as a defect in the backend under test.
 *
 * Giving every instance the same count is what the QSD branch below does too,
 * and it is safe: hw/block/vhost-user-blk.c advertises VIRTIO_BLK_F_MQ only
 * when the DEVICE's num-queues exceeds 1, and starts only that many vrings, so
 * queues a device never asked for go unused. multiqueue depends on exactly that
 * -- one setup call covers both its devices, yet it asserts the primary does
 * NOT offer VIRTIO_BLK_F_MQ while the one it hotplugs with num-queues=8 does,
 * and reads 8 back out of config space.
 */
static void start_photon_vhost_user_blk(GString *cmd_line, int vus_instances,
                                        int num_queues)
{
    const char *bin = getenv("PHOTON_VHU_BACKEND");
    char *queues = g_strdup_printf("%d", num_queues);
    int i;

    g_string_append_printf(cmd_line,
            " -object memory-backend-shm,id=mem,size=256M "
            " -M memory-backend=mem -m 256M ");

    /* in the parent rather than in the child below, for the reason the strings
     * there are built here: g_setenv allocates, and the child inherits it */
    if (!g_setenv("PHOTON_VHU_QUEUES", queues, true)) {
        fprintf(stderr, "cannot set PHOTON_VHU_QUEUES=%s\n", queues);
        abort();
    }
    g_free(queues);

    for (i = 0; i < vus_instances; i++) {
        char *sock_path = g_strdup_printf("%s/photon-vhu-%d-%d.sock",
                                          g_get_tmp_dir(), (int)getpid(), i);
        char *img_path = drive_create();
        /* built before the fork: allocating in the child of a multithreaded
         * process can deadlock on a lock another thread held at fork() */
        char *log_path = g_strdup_printf("%s/photon-vhu-%d-%d.log",
                                         g_get_tmp_dir(), (int)getpid(), i);
        GString *peer = g_string_new(NULL);
        QemuStorageDaemonState *qsd;
        pid_t pid;
        int t;

        g_string_append_printf(peer, "exec %s %s %s %d",
                               bin, sock_path, img_path, TEST_IMAGE_SIZE);
        g_test_message("starting vhost-user backend: %s (log %s)",
                       peer->str, log_path);

        pid = fork();
        if (pid == 0) {
            int lfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            close(0);
            close(1);
            close(2);
            open("/dev/null", O_RDONLY);
            if (lfd >= 0) {
                dup2(lfd, 1);
                dup2(lfd, 2);
                close(lfd);
            }
            execlp("/bin/sh", "sh", "-c", peer->str, NULL);
            exit(1);
        }
        g_string_free(peer, true);
        g_free(log_path);

        qsd = g_new(QemuStorageDaemonState, 1);
        qsd->pid = pid;
        qtest_add_abrt_handler(quit_storage_daemon, qsd);
        g_test_queue_destroy(quit_storage_daemon, qsd);
        g_test_queue_destroy(destroy_file, sock_path);

        for (t = 0; t < 500; t++) {
            struct sockaddr_un un;
            int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
            bool up;

            g_assert_cmpint(sizeof(un.sun_path), >, strlen(sock_path));
            memset(&un, 0, sizeof(un));
            un.sun_family = AF_UNIX;
            strcpy(un.sun_path, sock_path);
            up = cfd >= 0 && connect(cfd, (struct sockaddr *)&un, sizeof(un)) == 0;
            if (cfd >= 0) {
                close(cfd);
            }
            if (up) {
                break;
            }
            if (t == 499) {
                fprintf(stderr, "photon backend never listened on %s\n", sock_path);
                abort();
            }
            g_usleep(20 * 1000);
        }

        g_string_append_printf(cmd_line, "-chardev socket,id=char%d,path=%s ",
                               i + 1, sock_path);
    }
}

'''

DISPATCH_ANCHOR = ('    QemuStorageDaemonState *qsd;\n'
                   '\n'
                   '    g_string_append_printf(storage_daemon_command,\n')

DISPATCH = ('    QemuStorageDaemonState *qsd;\n'
            '\n'
            '    if (getenv("PHOTON_VHU_BACKEND")) {\n'
            '        g_string_free(storage_daemon_command, true);\n'
            '        start_photon_vhost_user_blk(cmd_line, vus_instances, '
            'num_queues);\n'
            '        return;\n'
            '    }\n'
            '\n'
            '    g_string_append_printf(storage_daemon_command,\n')


def sub_once(text, anchor, replacement, what):
    n = text.count(anchor)
    if n != 1:
        sys.exit("anchor for %s matched %d times, expected exactly 1 -- "
                 "this QEMU version differs from the one the script was "
                 "written against; nothing was written" % (what, n))
    return text.replace(anchor, replacement)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: %s <path/to/tests/qtest/vhost-user-blk-test.c>" % sys.argv[0])
    path = sys.argv[1]
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if "start_photon_vhost_user_blk" in text:
        print("already patched: %s" % path)
        return
    text = sub_once(text, INCLUDE_ANCHOR, INCLUDE_ANCHOR + INCLUDES, "includes")
    text = sub_once(text, FUNC_ANCHOR, FUNC + FUNC_ANCHOR, "photon peer function")
    text = sub_once(text, DISPATCH_ANCHOR, DISPATCH, "peer dispatch")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("patched: %s" % path)


if __name__ == "__main__":
    main()
