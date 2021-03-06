/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Disk.h"
#include "VolumeManager.h"
#include "CommandListener.h"
#ifndef MINIVOLD
#include "CryptCommandListener.h"
#endif
#include "NetlinkManager.h"
#include "cryptfs.h"
#include "sehandle.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/klog.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>
#include <fcntl.h>
#include <dirent.h>
#include <fs_mgr.h>

static int process_config(VolumeManager *vm, bool* has_adoptable);
static void coldboot(const char *path);
static void parse_args(int argc, char** argv);

struct fstab *fstab;

#ifdef MINIVOLD
extern struct selabel_handle *sehandle;
#else
struct selabel_handle *sehandle;
#endif

using android::base::StringPrintf;

extern "C" int vold_main(int argc, char** argv) {
    setenv("ANDROID_LOG_TAGS", "*:v", 1);
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));

    LOG(INFO) << "Vold 3.0 (the awakening) firing up";

    LOG(VERBOSE) << "Detected support for:"
            << (android::vold::IsFilesystemSupported("exfat") ? " exfat" : "")
            << (android::vold::IsFilesystemSupported("ext4") ? " ext4" : "")
            << (android::vold::IsFilesystemSupported("f2fs") ? " f2fs" : "")
            << (android::vold::IsFilesystemSupported("iso9660") ? " iso9660" : "")
            << (android::vold::IsFilesystemSupported("ntfs") ? " ntfs" : "")
            << (android::vold::IsFilesystemSupported("vfat") ? " vfat" : "");

    VolumeManager *vm;
    CommandListener *cl;
#ifndef MINIVOLD
    CryptCommandListener *ccl;
#endif
    NetlinkManager *nm;

    parse_args(argc, argv);

    sehandle = selinux_android_file_context_handle();
    if (sehandle) {
        selinux_android_set_sehandle(sehandle);
    }

    // Quickly throw a CLOEXEC on the socket we just inherited from init
    fcntl(android_get_control_socket("vold"), F_SETFD, FD_CLOEXEC);
    fcntl(android_get_control_socket("cryptd"), F_SETFD, FD_CLOEXEC);

    mkdir("/dev/block/vold", 0755);

    /* For when cryptfs checks and mounts an encrypted filesystem */
    klog_set_level(6);

    /* Create our singleton managers */
    if (!(vm = VolumeManager::Instance())) {
        LOG(ERROR) << "Unable to create VolumeManager";
        exit(1);
    }

    if (!(nm = NetlinkManager::Instance())) {
        LOG(ERROR) << "Unable to create NetlinkManager";
        exit(1);
    }

    if (property_get_bool("vold.debug", false)) {
        vm->setDebug(true);
    }

    cl = new CommandListener();
#ifndef MINIVOLD
    ccl = new CryptCommandListener();
#endif
    vm->setBroadcaster((SocketListener *) cl);
    nm->setBroadcaster((SocketListener *) cl);

    if (vm->start()) {
        PLOG(ERROR) << "Unable to start VolumeManager";
        exit(1);
    }

    bool has_adoptable;

    if (process_config(vm, &has_adoptable)) {
        PLOG(ERROR) << "Error reading configuration... continuing anyways";
    }

    if (nm->start()) {
        PLOG(ERROR) << "Unable to start NetlinkManager";
        exit(1);
    }

    coldboot("/sys/block");
//    coldboot("/sys/class/switch");

    /*
     * Now that we're up, we can respond to commands
     */
    if (cl->startListener()) {
        PLOG(ERROR) << "Unable to start CommandListener";
        exit(1);
    }

#ifndef MINIVOLD
    if (ccl->startListener()) {
        PLOG(ERROR) << "Unable to start CryptCommandListener";
        exit(1);
    }
#endif

    // This call should go after listeners are started to avoid
    // a deadlock between vold and init (see b/34278978 for details)
    property_set("vold.has_adoptable", has_adoptable ? "1" : "0");

    // Eventually we'll become the monitoring thread
    while(1) {
        sleep(1000);
    }

    LOG(ERROR) << "Vold exiting";
    exit(0);
}

static void parse_args(int argc, char** argv) {
    static struct option opts[] = {
        {"blkid_context", required_argument, 0, 'b' },
        {"blkid_untrusted_context", required_argument, 0, 'B' },
        {"fsck_context", required_argument, 0, 'f' },
        {"fsck_untrusted_context", required_argument, 0, 'F' },
    };

    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'b': android::vold::sBlkidContext = optarg; break;
        case 'B': android::vold::sBlkidUntrustedContext = optarg; break;
        case 'f': android::vold::sFsckContext = optarg; break;
        case 'F': android::vold::sFsckUntrustedContext = optarg; break;
        }
    }

    CHECK(android::vold::sBlkidContext != nullptr);
    CHECK(android::vold::sBlkidUntrustedContext != nullptr);
    CHECK(android::vold::sFsckContext != nullptr);
    CHECK(android::vold::sFsckUntrustedContext != nullptr);
}

static void do_coldboot(DIR *d, int lvl) {
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY | O_CLOEXEC);
    if(fd >= 0) {
        write(fd, "add\n", 4);
        close(fd);
    }

    while((de = readdir(d))) {
        DIR *d2;

        if (de->d_name[0] == '.')
            continue;

        if (de->d_type != DT_DIR && lvl > 0)
            continue;

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if(fd < 0)
            continue;

        d2 = fdopendir(fd);
        if(d2 == 0)
            close(fd);
        else {
            do_coldboot(d2, lvl + 1);
            closedir(d2);
        }
    }
}

static void coldboot(const char *path) {
    DIR *d = opendir(path);
    if(d) {
        do_coldboot(d, 0);
        closedir(d);
    }
}

static int process_config(VolumeManager *vm, bool* has_adoptable) {
    std::string path(android::vold::DefaultFstabPath());
    fstab = fs_mgr_read_fstab(path.c_str());
    if (!fstab) {
        PLOG(ERROR) << "Failed to open default fstab " << path;
        return -1;
    }

    /* Loop through entries looking for ones that vold manages */
    *has_adoptable = false;
    for (int i = 0; i < fstab->num_entries; i++) {
        if (fs_mgr_is_voldmanaged(&fstab->recs[i])) {
            std::string sysPattern(fstab->recs[i].blk_device);
            std::string fstype;
            if (fstab->recs[i].fs_type) {
                fstype = fstab->recs[i].fs_type;
            }
            std::string mntopts;
            if (fstab->recs[i].fs_options) {
                mntopts = fstab->recs[i].fs_options;
            }
            std::string nickname(fstab->recs[i].label);
            int partnum = fstab->recs[i].partnum;
            int flags = 0;

            if (fs_mgr_is_encryptable(&fstab->recs[i])) {
                flags |= android::vold::Disk::Flags::kAdoptable;
                *has_adoptable = true;
            }
            if (fs_mgr_is_noemulatedsd(&fstab->recs[i])
                    || property_get_bool("vold.debug.default_primary", false)) {
                flags |= android::vold::Disk::Flags::kDefaultPrimary;
            }
            if (fs_mgr_is_nonremovable(&fstab->recs[i])) {
                flags |= android::vold::Disk::Flags::kNonRemovable;
            }

            vm->addDiskSource(std::shared_ptr<VolumeManager::DiskSource>(
                    new VolumeManager::DiskSource(sysPattern, nickname, partnum, flags,
                                    fstype, mntopts)));
        }
    }

    if (android::base::ReadFileToString("/proc/cmdline", &path)) {
        size_t pos = path.find("SDCARD=");
        if (pos != std::string::npos) {
            std::string sdcard = path.substr(pos + 7);
            sdcard = sdcard.substr(0, sdcard.find_first_of(" \n"));
            if (!sdcard.empty()) {
                int partnum = -1;
                if (access(std::string("/sys/block/" + sdcard).c_str(), X_OK)) { // not a disk
                    auto d = std::find_if_not(sdcard.rbegin(), sdcard.rend(), ::isdigit);
                    pos = std::distance(d, sdcard.rend());
                    if (pos != sdcard.length()) {
                        partnum = std::stoi(sdcard.substr(pos));
                        sdcard = sdcard.substr(0, pos);
                        if (sdcard.find("mmcblk") != std::string::npos) {
                            // exclude the last 'p'
                            sdcard = sdcard.substr(0, pos - 1);
                        }
                        if (sdcard.find("nvme") != std::string::npos) {
                            // exclude the last 'p'
                            sdcard = sdcard.substr(0, pos - 1);
                        }
                    }
                }
                vm->addDiskSource(std::shared_ptr<VolumeManager::DiskSource>(
                        new VolumeManager::DiskSource("/devices/*/" + sdcard, sdcard,
                        partnum, android::vold::Disk::Flags::kAdoptable, "auto", "")));
                *has_adoptable = true;
                LOG(INFO) << "Add SDCARD=" << sdcard << " partnum=" << partnum;
            }
        }

    }

    return 0;
}
