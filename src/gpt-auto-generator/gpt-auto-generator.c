/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <blkid/blkid.h>
#include <stdlib.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "libudev.h"
#include "sd-id128.h"

#include "alloc-util.h"
#include "blkid-util.h"
#include "dirent-util.h"
#include "efivars.h"
#include "fd-util.h"
#include "fileio.h"
#include "fstab-util.h"
#include "generator.h"
#include "gpt.h"
#include "missing.h"
#include "mkdir.h"
#include "mount-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "stat-util.h"
#include "string-util.h"
#include "udev-util.h"
#include "unit-name.h"
#include "util.h"
#include "virt.h"

static const char *arg_dest = "/tmp";
static bool arg_enabled = true;
static bool arg_root_enabled = true;
static bool arg_root_rw = false;

static int add_cryptsetup(const char *id, const char *what, bool rw, char **device) {
        _cleanup_free_ char *e = NULL, *n = NULL, *p = NULL, *d = NULL, *to = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        char *from, *ret;
        int r;

        assert(id);
        assert(what);
        assert(device);

        r = unit_name_from_path(what, ".device", &d);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        e = unit_name_escape(id);
        if (!e)
                return log_oom();

        r = unit_name_build("systemd-cryptsetup", e, ".service", &n);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = strjoin(arg_dest, "/", n, NULL);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", p);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=Cryptography Setup for %%I\n"
                "Documentation=man:systemd-gpt-auto-generator(8) man:systemd-cryptsetup@.service(8)\n"
                "DefaultDependencies=no\n"
                "Conflicts=umount.target\n"
                "BindsTo=dev-mapper-%%i.device %s\n"
                "Before=umount.target cryptsetup.target\n"
                "After=%s\n"
                "IgnoreOnIsolate=true\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "TimeoutSec=0\n" /* the binary handles timeouts anyway */
                "ExecStart=" SYSTEMD_CRYPTSETUP_PATH " attach '%s' '%s' '' '%s'\n"
                "ExecStop=" SYSTEMD_CRYPTSETUP_PATH " detach '%s'\n",
                d, d,
                id, what, rw ? "" : "read-only",
                id);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write file %s: %m", p);

        from = strjoina("../", n);

        to = strjoin(arg_dest, "/", d, ".wants/", n, NULL);
        if (!to)
                return log_oom();

        mkdir_parents_label(to, 0755);
        if (symlink(from, to) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", to);

        free(to);
        to = strjoin(arg_dest, "/cryptsetup.target.requires/", n, NULL);
        if (!to)
                return log_oom();

        mkdir_parents_label(to, 0755);
        if (symlink(from, to) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", to);

        free(to);
        to = strjoin(arg_dest, "/dev-mapper-", e, ".device.requires/", n, NULL);
        if (!to)
                return log_oom();

        mkdir_parents_label(to, 0755);
        if (symlink(from, to) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", to);

        free(p);
        p = strjoin(arg_dest, "/dev-mapper-", e, ".device.d/50-job-timeout-sec-0.conf", NULL);
        if (!p)
                return log_oom();

        mkdir_parents_label(p, 0755);
        r = write_string_file(p,
                        "# Automatically generated by systemd-gpt-auto-generator\n\n"
                        "[Unit]\n"
                        "JobTimeoutSec=0\n",
                        WRITE_STRING_FILE_CREATE); /* the binary handles timeouts anyway */
        if (r < 0)
                return log_error_errno(r, "Failed to write device drop-in: %m");

        ret = strappend("/dev/mapper/", id);
        if (!ret)
                return log_oom();

        *device = ret;
        return 0;
}

static int add_mount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                const char *options,
                const char *description,
                const char *post) {

        _cleanup_free_ char *unit = NULL, *lnk = NULL, *crypto_what = NULL, *p = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(id);
        assert(what);
        assert(where);
        assert(description);

        log_debug("Adding %s: %s %s", where, what, strna(fstype));

        if (streq_ptr(fstype, "crypto_LUKS")) {

                r = add_cryptsetup(id, what, rw, &crypto_what);
                if (r < 0)
                        return r;

                what = crypto_what;
                fstype = NULL;
        }

        r = unit_name_from_path(where, ".mount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = strjoin(arg_dest, "/", unit, NULL);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=%s\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n",
                description);

        if (post)
                fprintf(f, "Before=%s\n", post);

        r = generator_write_fsck_deps(f, arg_dest, what, where, fstype);
        if (r < 0)
                return r;

        fprintf(f,
                "\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n",
                what, where);

        if (fstype)
                fprintf(f, "Type=%s\n", fstype);

        if (options)
                fprintf(f, "Options=%s,%s\n", options, rw ? "rw" : "ro");
        else
                fprintf(f, "Options=%s\n", rw ? "rw" : "ro");

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        if (post) {
                lnk = strjoin(arg_dest, "/", post, ".requires/", unit, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(p, lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);
        }

        return 0;
}

static bool path_is_busy(const char *where) {
        int r;

        /* already a mountpoint; generators run during reload */
        r = path_is_mount_point(where, AT_SYMLINK_FOLLOW);
        if (r > 0)
                return false;

        /* the directory might not exist on a stateless system */
        if (r == -ENOENT)
                return false;

        if (r < 0)
                return true;

        /* not a mountpoint but it contains files */
        if (dir_is_empty(where) <= 0)
                return true;

        return false;
}

static int probe_and_add_mount(
                const char *id,
                const char *what,
                const char *where,
                bool rw,
                const char *description,
                const char *post) {

        _cleanup_blkid_free_probe_ blkid_probe b = NULL;
        const char *fstype = NULL;
        int r;

        assert(id);
        assert(what);
        assert(where);
        assert(description);

        if (path_is_busy(where)) {
                log_debug("%s already populated, ignoring.", where);
                return 0;
        }

        /* Let's check the partition type here, so that we know
         * whether to do LUKS magic. */

        errno = 0;
        b = blkid_new_probe_from_filename(what);
        if (!b) {
                if (errno == 0)
                        return log_oom();
                return log_error_errno(errno, "Failed to allocate prober: %m");
        }

        blkid_probe_enable_superblocks(b, 1);
        blkid_probe_set_superblocks_flags(b, BLKID_SUBLKS_TYPE);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == -2 || r == 1) /* no result or uncertain */
                return 0;
        else if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe %s: %m", what);

        /* add_mount is OK with fstype being NULL. */
        (void) blkid_probe_lookup_value(b, "TYPE", &fstype, NULL);

        return add_mount(
                        id,
                        what,
                        where,
                        fstype,
                        rw,
                        NULL,
                        description,
                        post);
}

static int add_swap(const char *path) {
        _cleanup_free_ char *name = NULL, *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(path);

        /* Disable the swap auto logic if at least one swap is defined in /etc/fstab, see #6192. */
        r = fstab_has_fstype("swap");
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("swap specified in fstab, ignoring.");
                return 0;
        }

        log_debug("Adding swap: %s", path);

        r = unit_name_from_path(path, ".swap", &name);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=Swap Partition\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n\n"
                "[Swap]\n"
                "What=%s\n",
                path);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        lnk = strjoin(arg_dest, "/" SPECIAL_SWAP_TARGET ".wants/", name, NULL);
        if (!lnk)
                return log_oom();

        mkdir_parents_label(lnk, 0755);
        if (symlink(unit, lnk) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        return 0;
}

#ifdef ENABLE_EFI
static int add_automount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                const char *options,
                const char *description,
                usec_t timeout) {

        _cleanup_free_ char *unit = NULL, *lnk = NULL;
        _cleanup_free_ char *opt, *p = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(id);
        assert(where);
        assert(description);

        if (options)
                opt = strjoin(options, ",noauto", NULL);
        else
                opt = strdup("noauto");
        if (!opt)
                return log_oom();

        r = add_mount(id,
                      what,
                      where,
                      fstype,
                      rw,
                      opt,
                      description,
                      NULL);
        if (r < 0)
                return r;

        r = unit_name_from_path(where, ".automount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = strjoin(arg_dest, "/", unit, NULL);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=%s\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n"
                "[Automount]\n"
                "Where=%s\n"
                "TimeoutIdleSec=%lld\n",
                description,
                where,
                (unsigned long long)timeout / USEC_PER_SEC);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        lnk = strjoin(arg_dest, "/" SPECIAL_LOCAL_FS_TARGET ".wants/", unit, NULL);
        if (!lnk)
                return log_oom();
        mkdir_parents_label(lnk, 0755);

        if (symlink(p, lnk) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        return 0;
}

static int add_boot(const char *what) {
        _cleanup_blkid_free_probe_ blkid_probe b = NULL;
        const char *fstype = NULL, *uuid = NULL;
        sd_id128_t id, type_id;
        int r;

        assert(what);

        if (!is_efi_boot()) {
                log_debug("Not an EFI boot, ignoring /boot.");
                return 0;
        }

        if (in_initrd()) {
                log_debug("In initrd, ignoring /boot.");
                return 0;
        }

        if (detect_container() > 0) {
                log_debug("In a container, ignoring /boot.");
                return 0;
        }

        /* We create an .automount which is not overridden by the .mount from the fstab generator. */
        if (fstab_is_mount_point("/boot")) {
                log_debug("/boot specified in fstab, ignoring.");
                return 0;
        }

        if (path_is_busy("/boot")) {
                log_debug("/boot already populated, ignoring.");
                return 0;
        }

        r = efi_loader_get_device_part_uuid(&id);
        if (r == -ENOENT) {
                log_debug("EFI loader partition unknown.");
                return 0;
        }

        if (r < 0) {
                log_error_errno(r, "Failed to read ESP partition UUID: %m");
                return r;
        }

        errno = 0;
        b = blkid_new_probe_from_filename(what);
        if (!b) {
                if (errno == 0)
                        return log_oom();
                return log_error_errno(errno, "Failed to allocate prober: %m");
        }

        blkid_probe_enable_partitions(b, 1);
        blkid_probe_set_partitions_flags(b, BLKID_PARTS_ENTRY_DETAILS);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == -2 || r == 1) /* no result or uncertain */
                return 0;
        else if (r != 0)
                return log_error_errno(errno ?: EIO, "Failed to probe %s: %m", what);

        (void) blkid_probe_lookup_value(b, "TYPE", &fstype, NULL);
        if (!streq(fstype, "vfat")) {
                log_debug("Partition for /boot is not a FAT filesystem, ignoring.");
                return 0;
        }

        r = blkid_probe_lookup_value(b, "PART_ENTRY_UUID", &uuid, NULL);
        if (r != 0) {
                log_debug_errno(r, "Partition for /boot does not have a UUID, ignoring. %m");
                return 0;
        }

        if (sd_id128_from_string(uuid, &type_id) < 0) {
                log_debug("Partition for /boot does not have a valid UUID, ignoring.");
                return 0;
        }

        if (!sd_id128_equal(type_id, id)) {
                log_debug("Partition for /boot does not appear to be the partition we are booted from.");
                return 0;
        }

        r = add_automount("boot",
                       what,
                       "/boot",
                       "vfat",
                       true,
                       "umask=0077",
                       "EFI System Partition Automount",
                       120 * USEC_PER_SEC);

        return r;
}
#else
static int add_boot(const char *what) {
        return 0;
}
#endif

static int enumerate_partitions(dev_t devnum) {

        _cleanup_udev_enumerate_unref_ struct udev_enumerate *e = NULL;
        _cleanup_udev_device_unref_ struct udev_device *d = NULL;
        _cleanup_blkid_free_probe_ blkid_probe b = NULL;
        _cleanup_udev_unref_ struct udev *udev = NULL;
        _cleanup_free_ char *boot = NULL, *home = NULL, *srv = NULL;
        struct udev_list_entry *first, *item;
        struct udev_device *parent = NULL;
        const char *name, *node, *pttype, *devtype;
        int boot_nr = -1, home_nr = -1, srv_nr = -1;
        bool home_rw = true, srv_rw = true;
        blkid_partlist pl;
        int r, k;
        dev_t pn;

        udev = udev_new();
        if (!udev)
                return log_oom();

        d = udev_device_new_from_devnum(udev, 'b', devnum);
        if (!d)
                return log_oom();

        name = udev_device_get_devnode(d);
        if (!name)
                name = udev_device_get_syspath(d);
        if (!name) {
                log_debug("Device %u:%u does not have a name, ignoring.",
                          major(devnum), minor(devnum));
                return 0;
        }

        parent = udev_device_get_parent(d);
        if (!parent) {
                log_debug("%s: not a partitioned device, ignoring.", name);
                return 0;
        }

        /* Does it have a devtype? */
        devtype = udev_device_get_devtype(parent);
        if (!devtype) {
                log_debug("%s: parent doesn't have a device type, ignoring.", name);
                return 0;
        }

        /* Is this a disk or a partition? We only care for disks... */
        if (!streq(devtype, "disk")) {
                log_debug("%s: parent isn't a raw disk, ignoring.", name);
                return 0;
        }

        /* Does it have a device node? */
        node = udev_device_get_devnode(parent);
        if (!node) {
                log_debug("%s: parent device does not have device node, ignoring.", name);
                return 0;
        }

        log_debug("%s: root device %s.", name, node);

        pn = udev_device_get_devnum(parent);
        if (major(pn) == 0)
                return 0;

        errno = 0;
        b = blkid_new_probe_from_filename(node);
        if (!b) {
                if (errno == 0)
                        return log_oom();

                return log_error_errno(errno, "%s: failed to allocate prober: %m", node);
        }

        blkid_probe_enable_partitions(b, 1);
        blkid_probe_set_partitions_flags(b, BLKID_PARTS_ENTRY_DETAILS);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == 1)
                return 0; /* no results */
        else if (r == -2) {
                log_warning("%s: probe gave ambiguous results, ignoring", node);
                return 0;
        } else if (r != 0)
                return log_error_errno(errno ?: EIO, "%s: failed to probe: %m", node);

        errno = 0;
        r = blkid_probe_lookup_value(b, "PTTYPE", &pttype, NULL);
        if (r != 0)
                return log_error_errno(errno ?: EIO,
                                       "%s: failed to determine partition table type: %m", node);

        /* We only do this all for GPT... */
        if (!streq_ptr(pttype, "gpt")) {
                log_debug("%s: not a GPT partition table, ignoring.", node);
                return 0;
        }

        errno = 0;
        pl = blkid_probe_get_partitions(b);
        if (!pl) {
                if (errno == 0)
                        return log_oom();

                return log_error_errno(errno, "%s: failed to list partitions: %m", node);
        }

        e = udev_enumerate_new(udev);
        if (!e)
                return log_oom();

        r = udev_enumerate_add_match_parent(e, parent);
        if (r < 0)
                return log_oom();

        r = udev_enumerate_add_match_subsystem(e, "block");
        if (r < 0)
                return log_oom();

        r = udev_enumerate_scan_devices(e);
        if (r < 0)
                return log_error_errno(r, "%s: failed to enumerate partitions: %m", node);

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_udev_device_unref_ struct udev_device *q;
                unsigned long long flags;
                const char *stype, *subnode;
                sd_id128_t type_id;
                blkid_partition pp;
                dev_t qn;
                int nr;

                q = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                if (!q)
                        continue;

                qn = udev_device_get_devnum(q);
                if (major(qn) == 0)
                        continue;

                if (qn == devnum)
                        continue;

                if (qn == pn)
                        continue;

                subnode = udev_device_get_devnode(q);
                if (!subnode)
                        continue;

                pp = blkid_partlist_devno_to_partition(pl, qn);
                if (!pp)
                        continue;

                nr = blkid_partition_get_partno(pp);
                if (nr < 0)
                        continue;

                stype = blkid_partition_get_type_string(pp);
                if (!stype)
                        continue;

                if (sd_id128_from_string(stype, &type_id) < 0)
                        continue;

                flags = blkid_partition_get_flags(pp);

                if (sd_id128_equal(type_id, GPT_SWAP)) {

                        if (flags & GPT_FLAG_NO_AUTO)
                                continue;

                        if (flags & GPT_FLAG_READ_ONLY) {
                                log_debug("%s marked as read-only swap partition, which is bogus. Ignoring.", subnode);
                                continue;
                        }

                        k = add_swap(subnode);
                        if (k < 0)
                                r = k;

                } else if (sd_id128_equal(type_id, GPT_ESP)) {

                        /* We only care for the first /boot partition */
                        if (boot && nr >= boot_nr)
                                continue;

                        /* Note that we do not honour the "no-auto"
                         * flag for the ESP, as it is often unset, to
                         * hide it from Windows. */

                        boot_nr = nr;

                        r = free_and_strdup(&boot, subnode);
                        if (r < 0)
                                return log_oom();

                } else if (sd_id128_equal(type_id, GPT_HOME)) {

                        if (flags & GPT_FLAG_NO_AUTO)
                                continue;

                        /* We only care for the first /home partition */
                        if (home && nr >= home_nr)
                                continue;

                        home_nr = nr;
                        home_rw = !(flags & GPT_FLAG_READ_ONLY),

                        r = free_and_strdup(&home, subnode);
                        if (r < 0)
                                return log_oom();

                } else if (sd_id128_equal(type_id, GPT_SRV)) {

                        if (flags & GPT_FLAG_NO_AUTO)
                                continue;

                        /* We only care for the first /srv partition */
                        if (srv && nr >= srv_nr)
                                continue;

                        srv_nr = nr;
                        srv_rw = !(flags & GPT_FLAG_READ_ONLY),

                        r = free_and_strdup(&srv, subnode);
                        if (r < 0)
                                return log_oom();
                }
        }

        if (boot) {
                k = add_boot(boot);
                if (k < 0)
                        r = k;
        }

        if (home) {
                k = probe_and_add_mount("home", home, "/home", home_rw, "Home Partition", SPECIAL_LOCAL_FS_TARGET);
                if (k < 0)
                        r = k;
        }

        if (srv) {
                k = probe_and_add_mount("srv", srv, "/srv", srv_rw, "Server Data Partition", SPECIAL_LOCAL_FS_TARGET);
                if (k < 0)
                        r = k;
        }

        return r;
}

static int parse_proc_cmdline_item(const char *key, const char *value) {
        int r;

        assert(key);

        if (STR_IN_SET(key, "systemd.gpt_auto", "rd.systemd.gpt_auto") && value) {

                r = parse_boolean(value);
                if (r < 0)
                        log_warning("Failed to parse gpt-auto switch \"%s\". Ignoring.", value);
                else
                        arg_enabled = r;

        } else if (streq(key, "root") && value) {

                /* Disable root disk logic if there's a root= value
                 * specified (unless it happens to be "gpt-auto") */

                arg_root_enabled = streq(value, "gpt-auto");

        } else if (streq(key, "rw") && !value)
                arg_root_rw = true;
        else if (streq(key, "ro") && !value)
                arg_root_rw = false;

        return 0;
}

static int add_root_mount(void) {

#ifdef ENABLE_EFI
        int r;

        if (!is_efi_boot()) {
                log_debug("Not a EFI boot, not creating root mount.");
                return 0;
        }

        r = efi_loader_get_device_part_uuid(NULL);
        if (r == -ENOENT) {
                log_debug("EFI loader partition unknown, exiting.");
                return 0;
        } else if (r < 0)
                return log_error_errno(r, "Failed to read ESP partition UUID: %m");

        /* OK, we have an ESP partition, this is fantastic, so let's
         * wait for a root device to show up. A udev rule will create
         * the link for us under the right name. */

        return add_mount(
                        "root",
                        "/dev/gpt-auto-root",
                        in_initrd() ? "/sysroot" : "/",
                        NULL,
                        arg_root_rw,
                        NULL,
                        "Root Partition",
                        in_initrd() ? SPECIAL_INITRD_ROOT_FS_TARGET : SPECIAL_LOCAL_FS_TARGET);
#else
        return 0;
#endif
}

static int add_mounts(void) {
        dev_t devno;
        int r;

        r = get_block_device_harder("/", &devno);
        if (r < 0)
                return log_error_errno(r, "Failed to determine block device of root file system: %m");
        else if (r == 0) {
                r = get_block_device_harder("/usr", &devno);
                if (r < 0)
                        return log_error_errno(r, "Failed to determine block device of /usr file system: %m");
                else if (r == 0) {
                        log_debug("Neither root nor /usr file system are on a (single) block device.");
                        return 0;
                }
        }

        return enumerate_partitions(devno);
}

int main(int argc, char *argv[]) {
        int r = 0;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[3];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        if (detect_container() > 0) {
                log_debug("In a container, exiting.");
                return EXIT_SUCCESS;
        }

        r = parse_proc_cmdline(parse_proc_cmdline_item);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (!arg_enabled) {
                log_debug("Disabled, exiting.");
                return EXIT_SUCCESS;
        }

        if (arg_root_enabled)
                r = add_root_mount();

        if (!in_initrd()) {
                int k;

                k = add_mounts();
                if (k < 0)
                        r = k;
        }

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
