/*
 * qemu_monitor.c: interaction with QEMU monitor console
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <poll.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include "qemu_monitor.h"
#include "qemu_monitor_text.h"
#include "qemu_conf.h"
#include "event.h"
#include "virterror_internal.h"
#include "memory.h"
#include "logging.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

struct _qemuMonitor {
    virMutex lock;

    int fd;
    int watch;
    int hasSendFD;

    virDomainObjPtr vm;

    qemuMonitorEOFNotify eofCB;
    qemuMonitorDiskSecretLookup secretCB;
};

void qemuMonitorLock(qemuMonitorPtr mon)
{
    virMutexLock(&mon->lock);
}

void qemuMonitorUnlock(qemuMonitorPtr mon)
{
    virMutexUnlock(&mon->lock);
}

/* Return -1 for error, 1 to continue reading and 0 for success */
typedef int qemuMonitorHandleOutput(virDomainObjPtr vm,
                                    const char *output);

/*
 * Returns -1 for error, 0 on end-of-file, 1 for success
 */
static int
qemuMonitorReadOutput(virDomainObjPtr vm,
                      int fd,
                      char *buf,
                      size_t buflen,
                      qemuMonitorHandleOutput func,
                      const char *what,
                      int timeout)
{
    size_t got = 0;
    buf[0] = '\0';
    timeout *= 1000; /* poll wants milli seconds */

    /* Consume & discard the initial greeting */
    while (got < (buflen-1)) {
        ssize_t ret;

        ret = read(fd, buf+got, buflen-got-1);

        if (ret < 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            if (errno == EINTR)
                continue;

            if (errno != EAGAIN) {
                virReportSystemError(NULL, errno,
                                     _("Failure while reading %s startup output"),
                                     what);
                return -1;
            }

            ret = poll(&pfd, 1, timeout);
            if (ret == 0) {
                qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 _("Timed out while reading %s startup output"), what);
                return -1;
            } else if (ret == -1) {
                if (errno != EINTR) {
                    virReportSystemError(NULL, errno,
                                         _("Failure while reading %s startup output"),
                                         what);
                    return -1;
                }
            } else {
                /* Make sure we continue loop & read any further data
                   available before dealing with EOF */
                if (pfd.revents & (POLLIN | POLLHUP))
                    continue;

                qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 _("Failure while reading %s startup output"), what);
                return -1;
            }
        } else if (ret == 0) {
            return 0;
        } else {
            got += ret;
            buf[got] = '\0';
            ret = func(vm, buf);
            if (ret == -1)
                return -1;
            if (ret == 1)
                continue;
            return 1;
        }
    }

    qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                     _("Out of space while reading %s startup output"), what);
    return -1;

}

static int
qemuMonitorCheckPrompt(virDomainObjPtr vm ATTRIBUTE_UNUSED,
                       const char *output)
{
    if (strstr(output, "(qemu) ") == NULL)
        return 1; /* keep reading */

    return 0;
}

static int
qemuMonitorOpenCommon(virDomainObjPtr vm,
                      int monfd,
                      int reconnect)
{
    char buf[1024];
    int ret;

    if (virSetCloseExec(monfd) < 0) {
        qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "%s", _("Unable to set monitor close-on-exec flag"));
        return -1;
    }
    if (virSetNonBlock(monfd) < 0) {
        qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "%s", _("Unable to put monitor into non-blocking mode"));
        return -1;
    }

    if (!reconnect) {
        if (qemuMonitorReadOutput(vm, monfd,
                                  buf, sizeof(buf),
                                  qemuMonitorCheckPrompt,
                                  "monitor", 10) <= 0)
            ret = -1;
        else
            ret = 0;
    } else {
        ret = 0;
    }

    return ret;
}

static int
qemuMonitorOpenUnix(virDomainObjPtr vm,
                    const char *monitor,
                    int reconnect)
{
    struct sockaddr_un addr;
    int monfd;
    int timeout = 3; /* In seconds */
    int ret, i = 0;

    if ((monfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(NULL, errno,
                             "%s", _("failed to create socket"));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, monitor) == NULL) {
        qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         _("Monitor path %s too big for destination"), monitor);
        goto error;
    }

    do {
        ret = connect(monfd, (struct sockaddr *) &addr, sizeof(addr));

        if (ret == 0)
            break;

        if (errno == ENOENT || errno == ECONNREFUSED) {
            /* ENOENT       : Socket may not have shown up yet
             * ECONNREFUSED : Leftover socket hasn't been removed yet */
            continue;
        }

        virReportSystemError(NULL, errno, "%s",
                             _("failed to connect to monitor socket"));
        goto error;

    } while ((++i <= timeout*5) && (usleep(.2 * 1000000) <= 0));

    if (ret != 0) {
        virReportSystemError(NULL, errno, "%s",
                             _("monitor socket did not show up."));
        goto error;
    }

    if (qemuMonitorOpenCommon(vm, monfd, reconnect) < 0)
        goto error;

    return monfd;

error:
    close(monfd);
    return -1;
}

static int
qemuMonitorOpenPty(virDomainObjPtr vm,
                   const char *monitor,
                   int reconnect)
{
    int monfd;

    if ((monfd = open(monitor, O_RDWR)) < 0) {
        qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         _("Unable to open monitor path %s"), monitor);
        return -1;
    }

    if (qemuMonitorOpenCommon(vm, monfd, reconnect) < 0)
        goto error;

    return monfd;

error:
    close(monfd);
    return -1;
}


static void
qemuMonitorIO(int watch, int fd, int events, void *opaque) {
    qemuMonitorPtr mon = opaque;
    int quit = 0, failed = 0;

    if (mon->fd != fd || mon->watch != watch) {
        VIR_ERROR0(_("event from unexpected fd/watch"));
        failed = 1;
    } else {
        if (events & (VIR_EVENT_HANDLE_HANGUP | VIR_EVENT_HANDLE_ERROR))
            quit = 1;
        else {
            VIR_ERROR(_("unhandled fd event %d for monitor fd %d"),
                      events, mon->fd);
            failed = 1;
        }
    }

    mon->eofCB(mon, mon->vm, failed);
}





qemuMonitorPtr
qemuMonitorOpen(virDomainObjPtr vm,
                int reconnect,
                qemuMonitorEOFNotify eofCB)
{
    qemuMonitorPtr mon;

    if (VIR_ALLOC(mon) < 0) {
        virReportOOMError(NULL);
        return NULL;
    }

    if (virMutexInit(&mon->lock) < 0) {
        qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s",
                         _("cannot initialize monitor mutex"));
        VIR_FREE(mon);
        return NULL;
    }
    mon->fd = -1;
    mon->vm = vm;
    mon->eofCB = eofCB;

    switch (vm->monitor_chr->type) {
    case VIR_DOMAIN_CHR_TYPE_UNIX:
        mon->hasSendFD = 1;
        mon->fd = qemuMonitorOpenUnix(vm, vm->monitor_chr->data.nix.path,
                                      reconnect);
        break;

    case VIR_DOMAIN_CHR_TYPE_PTY:
        mon->fd = qemuMonitorOpenPty(vm, vm->monitor_chr->data.file.path,
                                     reconnect);
        break;

    default:
        qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         _("unable to handle monitor type: %s"),
                         virDomainChrTypeToString(vm->monitor_chr->type));
        goto cleanup;
    }

    if ((mon->watch = virEventAddHandle(mon->fd,
                                        VIR_EVENT_HANDLE_HANGUP | VIR_EVENT_HANDLE_ERROR,
                                        qemuMonitorIO,
                                        mon, NULL)) < 0) {
        qemudReportError(NULL, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s",
                         _("unable to register monitor events"));
        goto cleanup;
    }


    return mon;

cleanup:
    qemuMonitorClose(mon);
    return NULL;
}


void qemuMonitorClose(qemuMonitorPtr mon)
{
    if (!mon)
        return;

    if (mon->watch)
        virEventRemoveHandle(mon->watch);

    if (mon->fd != -1)
        close(mon->fd);
    virMutexDestroy(&mon->lock);
    VIR_FREE(mon);
}


void qemuMonitorRegisterDiskSecretLookup(qemuMonitorPtr mon,
                                         qemuMonitorDiskSecretLookup secretCB)
{
    mon->secretCB = secretCB;
}


int qemuMonitorWrite(qemuMonitorPtr mon,
                     const char *data,
                     size_t len)
{
    return safewrite(mon->fd, data, len);
}

int qemuMonitorWriteWithFD(qemuMonitorPtr mon,
                           const char *data,
                           size_t len,
                           int fd)
{
    struct msghdr msg;
    struct iovec iov[1];
    ssize_t ret;
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;

    if (!mon->hasSendFD) {
        errno = EINVAL;
        return -1;
    }

    memset(&msg, 0, sizeof(msg));

    iov[0].iov_base = (void *)data;
    iov[0].iov_len = len;

    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    do {
        ret = sendmsg(mon->fd, &msg, 0);
    } while (ret < 0 && errno == EINTR);

    return ret == len ? 0 : -1;
}

int qemuMonitorRead(qemuMonitorPtr mon,
                    char *data,
                    size_t len)
{
    return read(mon->fd, data, len);
}

int qemuMonitorWaitForInput(qemuMonitorPtr mon)
{
    struct pollfd fd = { mon->fd, POLLIN | POLLERR | POLLHUP, 0 };

retry:
    if (poll(&fd, 1, -1) < 0) {
        if (errno == EINTR)
            goto retry;
        return -1;
    }
    return 0;
}


int qemuMonitorGetDiskSecret(qemuMonitorPtr mon,
                             virConnectPtr conn,
                             const char *path,
                             char **secret,
                             size_t *secretLen)
{
    *secret = NULL;
    *secretLen = 0;

    return mon->secretCB(mon, conn, mon->vm, path, secret, secretLen);
}


int
qemuMonitorStartCPUs(qemuMonitorPtr mon,
                     virConnectPtr conn)
{
    DEBUG("mon=%p, fd=%d", mon, mon->fd);

    return qemuMonitorTextStartCPUs(mon, conn);
}


int
qemuMonitorStopCPUs(qemuMonitorPtr mon)
{
    DEBUG("mon=%p, fd=%d", mon, mon->fd);

    return qemuMonitorTextStopCPUs(mon);
}


int qemuMonitorSystemPowerdown(qemuMonitorPtr mon)
{
    DEBUG("mon=%p, fd=%d", mon, mon->fd);

    return qemuMonitorTextSystemPowerdown(mon);
}


int qemuMonitorGetCPUInfo(qemuMonitorPtr mon,
                          int **pids)
{
    DEBUG("mon=%p, fd=%d", mon, mon->fd);

    return qemuMonitorTextGetCPUInfo(mon, pids);
}

int qemuMonitorGetBalloonInfo(qemuMonitorPtr mon,
                              unsigned long *currmem)
{
    DEBUG("mon=%p, fd=%d", mon, mon->fd);

    return qemuMonitorTextGetBalloonInfo(mon, currmem);
}


int qemuMonitorGetBlockStatsInfo(qemuMonitorPtr mon,
                                 const char *devname,
                                 long long *rd_req,
                                 long long *rd_bytes,
                                 long long *wr_req,
                                 long long *wr_bytes,
                                 long long *errs)
{
    DEBUG("mon=%p, fd=%d dev=%s", mon, mon->fd, devname);

    return qemuMonitorTextGetBlockStatsInfo(mon, devname,
                                            rd_req, rd_bytes,
                                            wr_req, wr_bytes,
                                            errs);
}


int qemuMonitorSetVNCPassword(qemuMonitorPtr mon,
                              const char *password)
{
    DEBUG("mon=%p, fd=%d", mon, mon->fd);

    return qemuMonitorTextSetVNCPassword(mon, password);
}


int qemuMonitorSetBalloon(qemuMonitorPtr mon,
                          unsigned long newmem)
{
    DEBUG("mon=%p, fd=%d newmem=%lu", mon, mon->fd, newmem);

    return qemuMonitorTextSetBalloon(mon, newmem);
}

int qemuMonitorEjectMedia(qemuMonitorPtr mon,
                          const char *devname)
{
    DEBUG("mon=%p, fd=%d devname=%s", mon, mon->fd, devname);

    return qemuMonitorTextEjectMedia(mon, devname);
}


int qemuMonitorChangeMedia(qemuMonitorPtr mon,
                           const char *devname,
                           const char *newmedia)
{
    DEBUG("mon=%p, fd=%d devname=%s newmedia=%s",
          mon, mon->fd, devname, newmedia);

    return qemuMonitorTextChangeMedia(mon, devname, newmedia);
}


int qemuMonitorSaveVirtualMemory(qemuMonitorPtr mon,
                                 unsigned long long offset,
                                 size_t length,
                                 const char *path)
{
    DEBUG("mon=%p, fd=%d offset=%llu length=%zu path=%s",
          mon, mon->fd, offset, length, path);

    return qemuMonitorTextSaveVirtualMemory(mon, offset, length, path);
}

int qemuMonitorSavePhysicalMemory(qemuMonitorPtr mon,
                                  unsigned long long offset,
                                  size_t length,
                                  const char *path)
{
    DEBUG("mon=%p, fd=%d offset=%llu length=%zu path=%s",
          mon, mon->fd, offset, length, path);

    return qemuMonitorTextSavePhysicalMemory(mon, offset, length, path);
}


int qemuMonitorSetMigrationSpeed(qemuMonitorPtr mon,
                                 unsigned long bandwidth)
{
    DEBUG("mon=%p, fd=%d bandwidth=%lu", mon, mon->fd, bandwidth);

    return qemuMonitorTextSetMigrationSpeed(mon, bandwidth);
}

int qemuMonitorGetMigrationStatus(qemuMonitorPtr mon,
                                  int *status,
                                  unsigned long long *transferred,
                                  unsigned long long *remaining,
                                  unsigned long long *total)
{
    DEBUG("mon=%p, fd=%d", mon, mon->fd);

    return qemuMonitorTextGetMigrationStatus(mon, status,
                                             transferred,
                                             remaining,
                                             total);
}


int qemuMonitorMigrateToHost(qemuMonitorPtr mon,
                             int background,
                             const char *hostname,
                             int port)
{
    DEBUG("mon=%p, fd=%d hostname=%s port=%d",
          mon, mon->fd, hostname, port);

    return qemuMonitorTextMigrateToHost(mon, background, hostname, port);
}


int qemuMonitorMigrateToCommand(qemuMonitorPtr mon,
                                int background,
                                const char * const *argv,
                                const char *target)
{
    DEBUG("mon=%p, fd=%d argv=%p target=%s",
          mon, mon->fd, argv, target);

    return qemuMonitorTextMigrateToCommand(mon, background, argv, target);
}

int qemuMonitorMigrateToUnix(qemuMonitorPtr mon,
                             int background,
                             const char *unixfile)
{
    DEBUG("mon=%p fd=%d unixfile=%s",
          mon, mon->fd, unixfile);

    return qemuMonitorTextMigrateToUnix(mon, background, unixfile);
}

int qemuMonitorMigrateCancel(qemuMonitorPtr mon)
{
    DEBUG("mon=%p fd=%d", mon, mon->fd);

    return qemuMonitorTextMigrateCancel(mon);
}

int qemuMonitorAddUSBDisk(qemuMonitorPtr mon,
                          const char *path)
{
    DEBUG("mon=%p, fd=%d path=%s", mon, mon->fd, path);

    return qemuMonitorTextAddUSBDisk(mon, path);
}


int qemuMonitorAddUSBDeviceExact(qemuMonitorPtr mon,
                                 int bus,
                                 int dev)
{
    DEBUG("mon=%p, fd=%d bus=%d dev=%d", mon, mon->fd, bus, dev);

    return qemuMonitorTextAddUSBDeviceExact(mon, bus, dev);
}

int qemuMonitorAddUSBDeviceMatch(qemuMonitorPtr mon,
                                 int vendor,
                                 int product)
{
    DEBUG("mon=%p, fd=%d vendor=%d product=%d",
          mon, mon->fd, vendor, product);

    return qemuMonitorTextAddUSBDeviceMatch(mon, vendor, product);
}


int qemuMonitorAddPCIHostDevice(qemuMonitorPtr mon,
                                unsigned hostDomain,
                                unsigned hostBus,
                                unsigned hostSlot,
                                unsigned hostFunction,
                                unsigned *guestDomain,
                                unsigned *guestBus,
                                unsigned *guestSlot)
{
    DEBUG("mon=%p, fd=%d domain=%d bus=%d slot=%d function=%d",
          mon, mon->fd,
          hostDomain, hostBus, hostSlot, hostFunction);

    return qemuMonitorTextAddPCIHostDevice(mon, hostDomain,
                                           hostBus, hostSlot,
                                           hostFunction,
                                           guestDomain,
                                           guestBus,
                                           guestSlot);
}


int qemuMonitorAddPCIDisk(qemuMonitorPtr mon,
                          const char *path,
                          const char *bus,
                          unsigned *guestDomain,
                          unsigned *guestBus,
                          unsigned *guestSlot)
{
    DEBUG("mon=%p, fd=%d path=%s bus=%s",
          mon, mon->fd, path, bus);

    return qemuMonitorTextAddPCIDisk(mon, path, bus,
                                     guestDomain, guestBus, guestSlot);
}


int qemuMonitorAddPCINetwork(qemuMonitorPtr mon,
                             const char *nicstr,
                             unsigned *guestDomain,
                             unsigned *guestBus,
                             unsigned *guestSlot)
{
    DEBUG("mon=%p, fd=%d nicstr=%s", mon, mon->fd, nicstr);

    return qemuMonitorTextAddPCINetwork(mon, nicstr, guestDomain,
                                        guestBus, guestSlot);
}


int qemuMonitorRemovePCIDevice(qemuMonitorPtr mon,
                               unsigned guestDomain,
                               unsigned guestBus,
                               unsigned guestSlot)
{
    DEBUG("mon=%p, fd=%d domain=%d bus=%d slot=%d",
          mon, mon->fd, guestDomain, guestBus, guestSlot);

    return qemuMonitorTextRemovePCIDevice(mon, guestDomain,
                                          guestBus, guestSlot);
}


int qemuMonitorSendFileHandle(qemuMonitorPtr mon,
                              const char *fdname,
                              int fd)
{
    DEBUG("mon=%p, fd=%d fdname=%s fd=%d",
          mon, mon->fd, fdname, fd);

    return qemuMonitorTextSendFileHandle(mon, fdname, fd);
}


int qemuMonitorCloseFileHandle(qemuMonitorPtr mon,
                               const char *fdname)
{
    DEBUG("mon=%p, fd=%d fdname=%s",
          mon, mon->fd, fdname);

    return qemuMonitorTextCloseFileHandle(mon, fdname);
}


int qemuMonitorAddHostNetwork(qemuMonitorPtr mon,
                              const char *netstr)
{
    DEBUG("mon=%p, fd=%d netstr=%s",
          mon, mon->fd, netstr);

    return qemuMonitorTextAddHostNetwork(mon, netstr);
}


int qemuMonitorRemoveHostNetwork(qemuMonitorPtr mon,
                                 int vlan,
                                 const char *netname)
{
    DEBUG("mon=%p, fd=%d netname=%s",
          mon, mon->fd, netname);

    return qemuMonitorTextRemoveHostNetwork(mon, vlan, netname);
}
