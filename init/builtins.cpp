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

#include "builtins.h"

#include <android/api-level.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <glob.h>
#include <linux/loop.h>
#include <linux/module.h>
#include <mntent.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <memory>

#include <ApexProperties.sysprop.h>
#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <bootloader_message/bootloader_message.h>
#include <cutils/android_reboot.h>
#include <fs_mgr.h>
#include <fscrypt/fscrypt.h>
#include <libgsi/libgsi.h>
#include <selinux/android.h>
#include <selinux/label.h>
#include <selinux/selinux.h>
#include <system/thread_defs.h>

#include "action_manager.h"
#include "bootchart.h"
#include "fscrypt_init_extensions.h"
#include "init.h"
#include "mount_namespace.h"
#include "parser.h"
#include "property_service.h"
#include "reboot.h"
#include "rlimit_parser.h"
#include "selabel.h"
#include "selinux.h"
#include "service.h"
#include "service_list.h"
#include "subcontext.h"
#include "util.h"

using namespace std::literals::string_literals;

using android::base::Basename;
using android::base::StartsWith;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::fs_mgr::Fstab;
using android::fs_mgr::ReadFstabFromFile;

#define chmod DO_NOT_USE_CHMOD_USE_FCHMODAT_SYMLINK_NOFOLLOW

namespace android {
namespace init {

// There are many legacy paths in rootdir/init.rc that will virtually never exist on a new
// device, such as '/sys/class/leds/jogball-backlight/brightness'.  As of this writing, there
// are 81 such failures on cuttlefish.  Instead of spamming the log reporting them, we do not
// report such failures unless we're running at the DEBUG log level.
class ErrorIgnoreEnoent {
  public:
    ErrorIgnoreEnoent()
        : ignore_error_(errno == ENOENT &&
                        android::base::GetMinimumLogSeverity() > android::base::DEBUG) {}
    explicit ErrorIgnoreEnoent(int errno_to_append)
        : error_(errno_to_append),
          ignore_error_(errno_to_append == ENOENT &&
                        android::base::GetMinimumLogSeverity() > android::base::DEBUG) {}

    template <typename T>
    operator android::base::expected<T, ResultError>() {
        if (ignore_error_) {
            return {};
        }
        return error_;
    }

    template <typename T>
    ErrorIgnoreEnoent& operator<<(T&& t) {
        error_ << t;
        return *this;
    }

  private:
    Error error_;
    bool ignore_error_;
};

inline ErrorIgnoreEnoent ErrnoErrorIgnoreEnoent() {
    return ErrorIgnoreEnoent(errno);
}

std::vector<std::string> late_import_paths;

static constexpr std::chrono::nanoseconds kCommandRetryTimeout = 5s;

static Result<void> reboot_into_recovery(const std::vector<std::string>& options) {
    LOG(ERROR) << "Rebooting into recovery";
    std::string err;
    if (!write_bootloader_message(options, &err)) {
        return Error() << "Failed to set bootloader message: " << err;
    }
    // This function should only be reached from init and not from vendor_init, and we want to
    // immediately trigger reboot instead of relaying through property_service.  Older devices may
    // still have paths that reach here from vendor_init, so we keep the property_set as a fallback.
    if (getpid() == 1) {
        TriggerShutdown("reboot,recovery");
    } else {
        property_set("sys.powerctl", "reboot,recovery");
    }
    return {};
}

template <typename F>
static void ForEachServiceInClass(const std::string& classname, F function) {
    for (const auto& service : ServiceList::GetInstance()) {
        if (service->classnames().count(classname)) std::invoke(function, service);
    }
}

static Result<void> do_class_start(const BuiltinArguments& args) {
    // Do not start a class if it has a property persist.dont_start_class.CLASS set to 1.
    if (android::base::GetBoolProperty("persist.init.dont_start_class." + args[1], false))
        return {};
    // Starting a class does not start services which are explicitly disabled.
    // They must  be started individually.
    for (const auto& service : ServiceList::GetInstance()) {
        if (service->classnames().count(args[1])) {
            if (auto result = service->StartIfNotDisabled(); !result) {
                LOG(ERROR) << "Could not start service '" << service->name()
                           << "' as part of class '" << args[1] << "': " << result.error();
            }
        }
    }
    return {};
}

static Result<void> do_class_start_post_data(const BuiltinArguments& args) {
    if (args.context != kInitContext) {
        return Error() << "command 'class_start_post_data' only available in init context";
    }
    static bool is_apex_updatable = android::sysprop::ApexProperties::updatable().value_or(false);

    if (!is_apex_updatable) {
        // No need to start these on devices that don't support APEX, since they're not
        // stopped either.
        return {};
    }
    for (const auto& service : ServiceList::GetInstance()) {
        if (service->classnames().count(args[1])) {
            if (auto result = service->StartIfPostData(); !result) {
                LOG(ERROR) << "Could not start service '" << service->name()
                           << "' as part of class '" << args[1] << "': " << result.error();
            }
        }
    }
    return {};
}

static Result<void> do_class_stop(const BuiltinArguments& args) {
    ForEachServiceInClass(args[1], &Service::Stop);
    return {};
}

static Result<void> do_class_reset(const BuiltinArguments& args) {
    ForEachServiceInClass(args[1], &Service::Reset);
    return {};
}

static Result<void> do_class_reset_post_data(const BuiltinArguments& args) {
    if (args.context != kInitContext) {
        return Error() << "command 'class_reset_post_data' only available in init context";
    }
    static bool is_apex_updatable = android::sysprop::ApexProperties::updatable().value_or(false);
    if (!is_apex_updatable) {
        // No need to stop these on devices that don't support APEX.
        return {};
    }
    ForEachServiceInClass(args[1], &Service::ResetIfPostData);
    return {};
}

static Result<void> do_class_restart(const BuiltinArguments& args) {
    // Do not restart a class if it has a property persist.dont_start_class.CLASS set to 1.
    if (android::base::GetBoolProperty("persist.init.dont_start_class." + args[1], false))
        return {};
    ForEachServiceInClass(args[1], &Service::Restart);
    return {};
}

static Result<void> do_domainname(const BuiltinArguments& args) {
    if (auto result = WriteFile("/proc/sys/kernel/domainname", args[1]); !result) {
        return Error() << "Unable to write to /proc/sys/kernel/domainname: " << result.error();
    }
    return {};
}

static Result<void> do_enable(const BuiltinArguments& args) {
    Service* svc = ServiceList::GetInstance().FindService(args[1]);
    if (!svc) return Error() << "Could not find service";

    if (auto result = svc->Enable(); !result) {
        return Error() << "Could not enable service: " << result.error();
    }

    return {};
}

static Result<void> do_exec(const BuiltinArguments& args) {
    auto service = Service::MakeTemporaryOneshotService(args.args);
    if (!service) {
        return Error() << "Could not create exec service: " << service.error();
    }
    if (auto result = (*service)->ExecStart(); !result) {
        return Error() << "Could not start exec service: " << result.error();
    }

    ServiceList::GetInstance().AddService(std::move(*service));
    return {};
}

static Result<void> do_exec_background(const BuiltinArguments& args) {
    auto service = Service::MakeTemporaryOneshotService(args.args);
    if (!service) {
        return Error() << "Could not create exec background service: " << service.error();
    }
    if (auto result = (*service)->Start(); !result) {
        return Error() << "Could not start exec background service: " << result.error();
    }

    ServiceList::GetInstance().AddService(std::move(*service));
    return {};
}

static Result<void> do_exec_start(const BuiltinArguments& args) {
    Service* service = ServiceList::GetInstance().FindService(args[1]);
    if (!service) {
        return Error() << "Service not found";
    }

    if (auto result = service->ExecStart(); !result) {
        return Error() << "Could not start exec service: " << result.error();
    }

    return {};
}

static Result<void> do_export(const BuiltinArguments& args) {
    if (setenv(args[1].c_str(), args[2].c_str(), 1) == -1) {
        return ErrnoError() << "setenv() failed";
    }
    return {};
}

static Result<void> do_hostname(const BuiltinArguments& args) {
    if (auto result = WriteFile("/proc/sys/kernel/hostname", args[1]); !result) {
        return Error() << "Unable to write to /proc/sys/kernel/hostname: " << result.error();
    }
    return {};
}

static Result<void> do_ifup(const BuiltinArguments& args) {
    struct ifreq ifr;

    strlcpy(ifr.ifr_name, args[1].c_str(), IFNAMSIZ);

    unique_fd s(TEMP_FAILURE_RETRY(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)));
    if (s < 0) return ErrnoError() << "opening socket failed";

    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
        return ErrnoError() << "ioctl(..., SIOCGIFFLAGS, ...) failed";
    }

    ifr.ifr_flags |= IFF_UP;

    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
        return ErrnoError() << "ioctl(..., SIOCSIFFLAGS, ...) failed";
    }

    return {};
}

static Result<void> do_insmod(const BuiltinArguments& args) {
    int flags = 0;
    auto it = args.begin() + 1;

    if (!(*it).compare("-f")) {
        flags = MODULE_INIT_IGNORE_VERMAGIC | MODULE_INIT_IGNORE_MODVERSIONS;
        it++;
    }

    std::string filename = *it++;
    std::string options = android::base::Join(std::vector<std::string>(it, args.end()), ' ');

    unique_fd fd(TEMP_FAILURE_RETRY(open(filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)));
    if (fd == -1) return ErrnoError() << "open(\"" << filename << "\") failed";

    int rc = syscall(__NR_finit_module, fd.get(), options.c_str(), flags);
    if (rc == -1) return ErrnoError() << "finit_module for \"" << filename << "\" failed";

    return {};
}

static Result<void> do_interface_restart(const BuiltinArguments& args) {
    Service* svc = ServiceList::GetInstance().FindInterface(args[1]);
    if (!svc) return Error() << "interface " << args[1] << " not found";
    svc->Restart();
    return {};
}

static Result<void> do_interface_start(const BuiltinArguments& args) {
    Service* svc = ServiceList::GetInstance().FindInterface(args[1]);
    if (!svc) return Error() << "interface " << args[1] << " not found";
    if (auto result = svc->Start(); !result) {
        return Error() << "Could not start interface: " << result.error();
    }
    return {};
}

static Result<void> do_interface_stop(const BuiltinArguments& args) {
    Service* svc = ServiceList::GetInstance().FindInterface(args[1]);
    if (!svc) return Error() << "interface " << args[1] << " not found";
    svc->Stop();
    return {};
}

// mkdir <path> [mode] [owner] [group] [<option> ...]
static Result<void> do_mkdir(const BuiltinArguments& args) {
    auto options = ParseMkdir(args.args);
    if (!options) return options.error();
    std::string ref_basename;
    if (options->ref_option == "ref") {
        ref_basename = fscrypt_key_ref;
    } else if (options->ref_option == "per_boot_ref") {
        ref_basename = fscrypt_key_per_boot_ref;
    } else {
        return Error() << "Unknown key option: '" << options->ref_option << "'";
    }

    struct stat mstat;
    if (lstat(options->target.c_str(), &mstat) != 0) {
        if (errno != ENOENT) {
            return ErrnoError() << "lstat() failed on " << options->target;
        }
        if (!make_dir(options->target, options->mode)) {
            return ErrnoErrorIgnoreEnoent() << "mkdir() failed on " << options->target;
        }
        if (lstat(options->target.c_str(), &mstat) != 0) {
            return ErrnoError() << "lstat() failed on new " << options->target;
        }
    }
    if (!S_ISDIR(mstat.st_mode)) {
        return Error() << "Not a directory on " << options->target;
    }
    bool needs_chmod = (mstat.st_mode & ~S_IFMT) != options->mode;
    if ((options->uid != static_cast<uid_t>(-1) && options->uid != mstat.st_uid) ||
        (options->gid != static_cast<gid_t>(-1) && options->gid != mstat.st_gid)) {
        if (lchown(options->target.c_str(), options->uid, options->gid) == -1) {
            return ErrnoError() << "lchown failed on " << options->target;
        }
        // chown may have cleared S_ISUID and S_ISGID, chmod again
        needs_chmod = true;
    }
    if (needs_chmod) {
        if (fchmodat(AT_FDCWD, options->target.c_str(), options->mode, AT_SYMLINK_NOFOLLOW) == -1) {
            return ErrnoError() << "fchmodat() failed on " << options->target;
        }
    }
    if (fscrypt_is_native()) {
        if (!FscryptSetDirectoryPolicy(ref_basename, options->fscrypt_action, options->target)) {
            return reboot_into_recovery(
                    {"--prompt_and_wipe_data", "--reason=set_policy_failed:"s + options->target});
        }
    }
    return {};
}

/* umount <path> */
static Result<void> do_umount(const BuiltinArguments& args) {
    if (umount(args[1].c_str()) < 0) {
        return ErrnoError() << "umount() failed";
    }
    return {};
}

static struct {
    const char *name;
    unsigned flag;
} mount_flags[] = {
    { "noatime",    MS_NOATIME },
    { "noexec",     MS_NOEXEC },
    { "nosuid",     MS_NOSUID },
    { "nodev",      MS_NODEV },
    { "nodiratime", MS_NODIRATIME },
    { "ro",         MS_RDONLY },
    { "rw",         0 },
    { "remount",    MS_REMOUNT },
    { "bind",       MS_BIND },
    { "rec",        MS_REC },
    { "unbindable", MS_UNBINDABLE },
    { "private",    MS_PRIVATE },
    { "slave",      MS_SLAVE },
    { "shared",     MS_SHARED },
    { "defaults",   0 },
    { 0,            0 },
};

#define DATA_MNT_POINT "/data"

/* mount <type> <device> <path> <flags ...> <options> */
static Result<void> do_mount(const BuiltinArguments& args) {
    const char* options = nullptr;
    unsigned flags = 0;
    bool wait = false;

    for (size_t na = 4; na < args.size(); na++) {
        size_t i;
        for (i = 0; mount_flags[i].name; i++) {
            if (!args[na].compare(mount_flags[i].name)) {
                flags |= mount_flags[i].flag;
                break;
            }
        }

        if (!mount_flags[i].name) {
            if (!args[na].compare("wait")) {
                wait = true;
                // If our last argument isn't a flag, wolf it up as an option string.
            } else if (na + 1 == args.size()) {
                options = args[na].c_str();
            }
        }
    }

    const char* system = args[1].c_str();
    const char* source = args[2].c_str();
    const char* target = args[3].c_str();

    if (android::base::StartsWith(source, "loop@")) {
        int mode = (flags & MS_RDONLY) ? O_RDONLY : O_RDWR;
        unique_fd fd(TEMP_FAILURE_RETRY(open(source + 5, mode | O_CLOEXEC)));
        if (fd < 0) return ErrnoError() << "open(" << source + 5 << ", " << mode << ") failed";

        for (size_t n = 0;; n++) {
            std::string tmp = android::base::StringPrintf("/dev/block/loop%zu", n);
            unique_fd loop(TEMP_FAILURE_RETRY(open(tmp.c_str(), mode | O_CLOEXEC)));
            if (loop < 0) return ErrnoError() << "open(" << tmp << ", " << mode << ") failed";

            loop_info info;
            /* if it is a blank loop device */
            if (ioctl(loop, LOOP_GET_STATUS, &info) < 0 && errno == ENXIO) {
                /* if it becomes our loop device */
                if (ioctl(loop, LOOP_SET_FD, fd.get()) >= 0) {
                    if (mount(tmp.c_str(), target, system, flags, options) < 0) {
                        ioctl(loop, LOOP_CLR_FD, 0);
                        return ErrnoError() << "mount() failed";
                    }
                    return {};
                }
            }
        }

        return Error() << "out of loopback devices";
    } else {
        if (wait)
            wait_for_file(source, kCommandRetryTimeout);
        if (mount(source, target, system, flags, options) < 0) {
            return ErrnoErrorIgnoreEnoent() << "mount() failed";
        }

    }

    return {};
}

/* Imports .rc files from the specified paths. Default ones are applied if none is given.
 *
 * start_index: index of the first path in the args list
 */
static void import_late(const std::vector<std::string>& args, size_t start_index, size_t end_index) {
    auto& action_manager = ActionManager::GetInstance();
    auto& service_list = ServiceList::GetInstance();
    Parser parser = CreateParser(action_manager, service_list);
    if (end_index <= start_index) {
        // Fallbacks for partitions on which early mount isn't enabled.
        for (const auto& path : late_import_paths) {
            parser.ParseConfig(path);
        }
        late_import_paths.clear();
    } else {
        for (size_t i = start_index; i < end_index; ++i) {
            parser.ParseConfig(args[i]);
        }
    }

    // Turning this on and letting the INFO logging be discarded adds 0.2s to
    // Nexus 9 boot time, so it's disabled by default.
    if (false) DumpState();
}

/* Queue event based on fs_mgr return code.
 *
 * code: return code of fs_mgr_mount_all
 *
 * This function might request a reboot, in which case it will
 * not return.
 *
 * return code is processed based on input code
 */
static Result<void> queue_fs_event(int code, bool userdata_remount) {
    if (code == FS_MGR_MNTALL_DEV_NEEDS_ENCRYPTION) {
        if (userdata_remount) {
            // FS_MGR_MNTALL_DEV_NEEDS_ENCRYPTION should only happen on FDE devices. Since we don't
            // support userdata remount on FDE devices, this should never been triggered. Time to
            // panic!
            LOG(ERROR) << "Userdata remount is not supported on FDE devices. How did you get here?";
            TriggerShutdown("reboot,requested-userdata-remount-on-fde-device");
        }
        ActionManager::GetInstance().QueueEventTrigger("encrypt");
        return {};
    } else if (code == FS_MGR_MNTALL_DEV_MIGHT_BE_ENCRYPTED) {
        if (userdata_remount) {
            // FS_MGR_MNTALL_DEV_MIGHT_BE_ENCRYPTED should only happen on FDE devices. Since we
            // don't support userdata remount on FDE devices, this should never been triggered.
            // Time to panic!
            LOG(ERROR) << "Userdata remount is not supported on FDE devices. How did you get here?";
            TriggerShutdown("reboot,requested-userdata-remount-on-fde-device");
        }
        property_set("ro.crypto.state", "encrypted");
        property_set("ro.crypto.type", "block");
        ActionManager::GetInstance().QueueEventTrigger("defaultcrypto");
        return {};
    } else if (code == FS_MGR_MNTALL_DEV_NOT_ENCRYPTED) {
        property_set("ro.crypto.state", "unencrypted");
        ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
        return {};
    } else if (code == FS_MGR_MNTALL_DEV_NOT_ENCRYPTABLE) {
        property_set("ro.crypto.state", "unsupported");
        ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
        return {};
    } else if (code == FS_MGR_MNTALL_DEV_NEEDS_RECOVERY) {
        /* Setup a wipe via recovery, and reboot into recovery */
        if (android::gsi::IsGsiRunning()) {
            return Error() << "cannot wipe within GSI";
        }
        PLOG(ERROR) << "fs_mgr_mount_all suggested recovery, so wiping data via recovery.";
        const std::vector<std::string> options = {"--wipe_data", "--reason=fs_mgr_mount_all" };
        return reboot_into_recovery(options);
        /* If reboot worked, there is no return. */
    } else if (code == FS_MGR_MNTALL_DEV_FILE_ENCRYPTED) {
        if (!userdata_remount && !FscryptInstallKeyring()) {
            return Error() << "FscryptInstallKeyring() failed";
        }
        property_set("ro.crypto.state", "encrypted");
        property_set("ro.crypto.type", "file");

        // Although encrypted, we have device key, so we do not need to
        // do anything different from the nonencrypted case.
        ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
        return {};
    } else if (code == FS_MGR_MNTALL_DEV_IS_METADATA_ENCRYPTED) {
        if (!userdata_remount && !FscryptInstallKeyring()) {
            return Error() << "FscryptInstallKeyring() failed";
        }
        property_set("ro.crypto.state", "encrypted");
        property_set("ro.crypto.type", "file");

        // Although encrypted, vold has already set the device up, so we do not need to
        // do anything different from the nonencrypted case.
        ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
        return {};
    } else if (code == FS_MGR_MNTALL_DEV_NEEDS_METADATA_ENCRYPTION) {
        if (!userdata_remount && !FscryptInstallKeyring()) {
            return Error() << "FscryptInstallKeyring() failed";
        }
        property_set("ro.crypto.state", "encrypted");
        property_set("ro.crypto.type", "file");

        // Although encrypted, vold has already set the device up, so we do not need to
        // do anything different from the nonencrypted case.
        ActionManager::GetInstance().QueueEventTrigger("nonencrypted");
        return {};
    } else if (code > 0) {
        Error() << "fs_mgr_mount_all() returned unexpected error " << code;
    }
    /* else ... < 0: error */

    return Error() << "Invalid code: " << code;
}

static int initial_mount_fstab_return_code = -1;

/* mount_all <fstab> [ <path> ]* [--<options>]*
 *
 * This function might request a reboot, in which case it will
 * not return.
 */
static Result<void> do_mount_all(const BuiltinArguments& args) {
    std::size_t na = 0;
    bool import_rc = true;
    bool queue_event = true;
    int mount_mode = MOUNT_MODE_DEFAULT;
    const auto& fstab_file = args[1];
    std::size_t path_arg_end = args.size();
    const char* prop_post_fix = "default";

    for (na = args.size() - 1; na > 1; --na) {
        if (args[na] == "--early") {
            path_arg_end = na;
            queue_event = false;
            mount_mode = MOUNT_MODE_EARLY;
            prop_post_fix = "early";
        } else if (args[na] == "--late") {
            path_arg_end = na;
            import_rc = false;
            mount_mode = MOUNT_MODE_LATE;
            prop_post_fix = "late";
        }
    }

    std::string prop_name = "ro.boottime.init.mount_all."s + prop_post_fix;
    android::base::Timer t;

    Fstab fstab;
    if (!ReadFstabFromFile(fstab_file, &fstab)) {
        return Error() << "Could not read fstab";
    }

    auto mount_fstab_return_code = fs_mgr_mount_all(&fstab, mount_mode);
    property_set(prop_name, std::to_string(t.duration().count()));

    if (import_rc && SelinuxGetVendorAndroidVersion() <= __ANDROID_API_Q__) {
        /* Paths of .rc files are specified at the 2nd argument and beyond */
        import_late(args.args, 2, path_arg_end);
    }

    if (queue_event) {
        /* queue_fs_event will queue event based on mount_fstab return code
         * and return processed return code*/
        initial_mount_fstab_return_code = mount_fstab_return_code;
        auto queue_fs_result = queue_fs_event(mount_fstab_return_code, false);
        if (!queue_fs_result) {
            return Error() << "queue_fs_event() failed: " << queue_fs_result.error();
        }
    }

    return {};
}

/* umount_all <fstab> */
static Result<void> do_umount_all(const BuiltinArguments& args) {
    Fstab fstab;
    if (!ReadFstabFromFile(args[1], &fstab)) {
        return Error() << "Could not read fstab";
    }

    if (auto result = fs_mgr_umount_all(&fstab); result != 0) {
        return Error() << "umount_fstab() failed " << result;
    }
    return {};
}

static Result<void> do_swapon_all(const BuiltinArguments& args) {
    Fstab fstab;
    if (!ReadFstabFromFile(args[1], &fstab)) {
        return Error() << "Could not read fstab '" << args[1] << "'";
    }

    if (!fs_mgr_swapon_all(fstab)) {
        return Error() << "fs_mgr_swapon_all() failed";
    }

    return {};
}

static Result<void> do_setprop(const BuiltinArguments& args) {
    if (StartsWith(args[1], "ctl.")) {
        return Error()
               << "Cannot set ctl. properties from init; call the Service functions directly";
    }
    if (args[1] == kRestoreconProperty) {
        return Error() << "Cannot set '" << kRestoreconProperty
                       << "' from init; use the restorecon builtin directly";
    }

    property_set(args[1], args[2]);
    return {};
}

static Result<void> do_setrlimit(const BuiltinArguments& args) {
    auto rlimit = ParseRlimit(args.args);
    if (!rlimit) return rlimit.error();

    if (setrlimit(rlimit->first, &rlimit->second) == -1) {
        return ErrnoError() << "setrlimit failed";
    }
    return {};
}

static Result<void> do_start(const BuiltinArguments& args) {
    Service* svc = ServiceList::GetInstance().FindService(args[1]);
    if (!svc) return Error() << "service " << args[1] << " not found";
    if (auto result = svc->Start(); !result) {
        return ErrorIgnoreEnoent() << "Could not start service: " << result.error();
    }
    return {};
}

static Result<void> do_stop(const BuiltinArguments& args) {
    Service* svc = ServiceList::GetInstance().FindService(args[1]);
    if (!svc) return Error() << "service " << args[1] << " not found";
    svc->Stop();
    return {};
}

static Result<void> do_restart(const BuiltinArguments& args) {
    Service* svc = ServiceList::GetInstance().FindService(args[1]);
    if (!svc) return Error() << "service " << args[1] << " not found";
    svc->Restart();
    return {};
}

static Result<void> do_trigger(const BuiltinArguments& args) {
    ActionManager::GetInstance().QueueEventTrigger(args[1]);
    return {};
}

static int MakeSymlink(const std::string& target, const std::string& linkpath) {
    std::string secontext;
    // Passing 0 for mode should work.
    if (SelabelLookupFileContext(linkpath, 0, &secontext) && !secontext.empty()) {
        setfscreatecon(secontext.c_str());
    }

    int rc = symlink(target.c_str(), linkpath.c_str());

    if (!secontext.empty()) {
        int save_errno = errno;
        setfscreatecon(nullptr);
        errno = save_errno;
    }

    return rc;
}

static Result<void> do_symlink(const BuiltinArguments& args) {
    if (MakeSymlink(args[1], args[2]) < 0) {
        // The symlink builtin is often used to create symlinks for older devices to be backwards
        // compatible with new paths, therefore we skip reporting this error.
        return ErrnoErrorIgnoreEnoent() << "symlink() failed";
    }
    return {};
}

static Result<void> do_rm(const BuiltinArguments& args) {
    if (unlink(args[1].c_str()) < 0) {
        return ErrnoError() << "unlink() failed";
    }
    return {};
}

static Result<void> do_rmdir(const BuiltinArguments& args) {
    if (rmdir(args[1].c_str()) < 0) {
        return ErrnoError() << "rmdir() failed";
    }
    return {};
}

static Result<void> do_sysclktz(const BuiltinArguments& args) {
    struct timezone tz = {};
    if (!android::base::ParseInt(args[1], &tz.tz_minuteswest)) {
        return Error() << "Unable to parse mins_west_of_gmt";
    }

    if (settimeofday(nullptr, &tz) == -1) {
        return ErrnoError() << "settimeofday() failed";
    }
    return {};
}

static Result<void> do_verity_update_state(const BuiltinArguments& args) {
    int mode;
    if (!fs_mgr_load_verity_state(&mode)) {
        return Error() << "fs_mgr_load_verity_state() failed";
    }

    Fstab fstab;
    if (!ReadDefaultFstab(&fstab)) {
        return Error() << "Failed to read default fstab";
    }

    for (const auto& entry : fstab) {
        if (!fs_mgr_is_verity_enabled(entry)) {
            continue;
        }

        // To be consistent in vboot 1.0 and vboot 2.0 (AVB), use "system" for the partition even
        // for system as root, so it has property [partition.system.verified].
        std::string partition = entry.mount_point == "/" ? "system" : Basename(entry.mount_point);
        property_set("partition." + partition + ".verified", std::to_string(mode));
    }

    return {};
}

static Result<void> do_write(const BuiltinArguments& args) {
    if (auto result = WriteFile(args[1], args[2]); !result) {
        return ErrorIgnoreEnoent()
               << "Unable to write to file '" << args[1] << "': " << result.error();
    }

    return {};
}

static Result<void> readahead_file(const std::string& filename, bool fully) {
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(filename.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd == -1) {
        return ErrnoError() << "Error opening file";
    }
    if (posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED)) {
        return ErrnoError() << "Error posix_fadvise file";
    }
    if (readahead(fd, 0, std::numeric_limits<size_t>::max())) {
        return ErrnoError() << "Error readahead file";
    }
    if (fully) {
        char buf[BUFSIZ];
        ssize_t n;
        while ((n = TEMP_FAILURE_RETRY(read(fd, &buf[0], sizeof(buf)))) > 0) {
        }
        if (n != 0) {
            return ErrnoError() << "Error reading file";
        }
    }
    return {};
}

static Result<void> do_readahead(const BuiltinArguments& args) {
    struct stat sb;

    if (stat(args[1].c_str(), &sb)) {
        return ErrnoError() << "Error opening " << args[1];
    }

    bool readfully = false;
    if (args.size() == 3 && args[2] == "--fully") {
        readfully = true;
    }
    // We will do readahead in a forked process in order not to block init
    // since it may block while it reads the
    // filesystem metadata needed to locate the requested blocks.  This
    // occurs frequently with ext[234] on large files using indirect blocks
    // instead of extents, giving the appearance that the call blocks until
    // the requested data has been read.
    pid_t pid = fork();
    if (pid == 0) {
        if (setpriority(PRIO_PROCESS, 0, static_cast<int>(ANDROID_PRIORITY_LOWEST)) != 0) {
            PLOG(WARNING) << "setpriority failed";
        }
        if (android_set_ioprio(0, IoSchedClass_IDLE, 7)) {
            PLOG(WARNING) << "ioprio_get failed";
        }
        android::base::Timer t;
        if (S_ISREG(sb.st_mode)) {
            if (auto result = readahead_file(args[1], readfully); !result) {
                LOG(WARNING) << "Unable to readahead '" << args[1] << "': " << result.error();
                _exit(EXIT_FAILURE);
            }
        } else if (S_ISDIR(sb.st_mode)) {
            char* paths[] = {const_cast<char*>(args[1].data()), nullptr};
            std::unique_ptr<FTS, decltype(&fts_close)> fts(
                fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr), fts_close);
            if (!fts) {
                PLOG(ERROR) << "Error opening directory: " << args[1];
                _exit(EXIT_FAILURE);
            }
            // Traverse the entire hierarchy and do readahead
            for (FTSENT* ftsent = fts_read(fts.get()); ftsent != nullptr;
                 ftsent = fts_read(fts.get())) {
                if (ftsent->fts_info & FTS_F) {
                    const std::string filename = ftsent->fts_accpath;
                    if (auto result = readahead_file(filename, readfully); !result) {
                        LOG(WARNING)
                            << "Unable to readahead '" << filename << "': " << result.error();
                    }
                }
            }
        }
        LOG(INFO) << "Readahead " << args[1] << " took " << t << " asynchronously";
        _exit(0);
    } else if (pid < 0) {
        return ErrnoError() << "Fork failed";
    }
    return {};
}

static Result<void> do_copy(const BuiltinArguments& args) {
    auto file_contents = ReadFile(args[1]);
    if (!file_contents) {
        return Error() << "Could not read input file '" << args[1] << "': " << file_contents.error();
    }
    if (auto result = WriteFile(args[2], *file_contents); !result) {
        return Error() << "Could not write to output file '" << args[2] << "': " << result.error();
    }

    return {};
}

static Result<void> do_chown(const BuiltinArguments& args) {
    auto uid = DecodeUid(args[1]);
    if (!uid) {
        return Error() << "Unable to decode UID for '" << args[1] << "': " << uid.error();
    }

    // GID is optional and pushes the index of path out by one if specified.
    const std::string& path = (args.size() == 4) ? args[3] : args[2];
    Result<gid_t> gid = -1;

    if (args.size() == 4) {
        gid = DecodeUid(args[2]);
        if (!gid) {
            return Error() << "Unable to decode GID for '" << args[2] << "': " << gid.error();
        }
    }

    if (lchown(path.c_str(), *uid, *gid) == -1) {
        return ErrnoErrorIgnoreEnoent() << "lchown() failed";
    }

    return {};
}

static mode_t get_mode(const char *s) {
    mode_t mode = 0;
    while (*s) {
        if (*s >= '0' && *s <= '7') {
            mode = (mode<<3) | (*s-'0');
        } else {
            return -1;
        }
        s++;
    }
    return mode;
}

static Result<void> do_chmod(const BuiltinArguments& args) {
    mode_t mode = get_mode(args[1].c_str());
    if (fchmodat(AT_FDCWD, args[2].c_str(), mode, AT_SYMLINK_NOFOLLOW) < 0) {
        return ErrnoErrorIgnoreEnoent() << "fchmodat() failed";
    }
    return {};
}

static Result<void> do_restorecon(const BuiltinArguments& args) {
    auto restorecon_info = ParseRestorecon(args.args);
    if (!restorecon_info) {
        return restorecon_info.error();
    }

    const auto& [flag, paths] = *restorecon_info;

    int ret = 0;
    for (const auto& path : paths) {
        if (selinux_android_restorecon(path.c_str(), flag) < 0) {
            ret = errno;
        }
    }

    if (ret) return ErrnoErrorIgnoreEnoent() << "selinux_android_restorecon() failed";
    return {};
}

static Result<void> do_restorecon_recursive(const BuiltinArguments& args) {
    std::vector<std::string> non_const_args(args.args);
    non_const_args.insert(std::next(non_const_args.begin()), "--recursive");
    return do_restorecon({std::move(non_const_args), args.context});
}

static Result<void> do_loglevel(const BuiltinArguments& args) {
    // TODO: support names instead/as well?
    int log_level = -1;
    android::base::ParseInt(args[1], &log_level);
    android::base::LogSeverity severity;
    switch (log_level) {
        case 7: severity = android::base::DEBUG; break;
        case 6: severity = android::base::INFO; break;
        case 5:
        case 4: severity = android::base::WARNING; break;
        case 3: severity = android::base::ERROR; break;
        case 2:
        case 1:
        case 0: severity = android::base::FATAL; break;
        default:
            return Error() << "invalid log level " << log_level;
    }
    android::base::SetMinimumLogSeverity(severity);
    return {};
}

static Result<void> do_load_persist_props(const BuiltinArguments& args) {
    // Devices with FDE have load_persist_props called twice; the first time when the temporary
    // /data partition is mounted and then again once /data is truly mounted.  We do not want to
    // read persistent properties from the temporary /data partition or mark persistent properties
    // as having been loaded during the first call, so we return in that case.
    std::string crypto_state = android::base::GetProperty("ro.crypto.state", "");
    std::string crypto_type = android::base::GetProperty("ro.crypto.type", "");
    if (crypto_state == "encrypted" && crypto_type == "block") {
        static size_t num_calls = 0;
        if (++num_calls == 1) return {};
    }

    SendLoadPersistentPropertiesMessage();

    start_waiting_for_property("ro.persistent_properties.ready", "true");
    return {};
}

static Result<void> do_load_system_props(const BuiltinArguments& args) {
    LOG(INFO) << "deprecated action `load_system_props` called.";
    return {};
}

static Result<void> do_wait(const BuiltinArguments& args) {
    auto timeout = kCommandRetryTimeout;
    if (args.size() == 3) {
        int timeout_int;
        if (!android::base::ParseInt(args[2], &timeout_int)) {
            return Error() << "failed to parse timeout";
        }
        timeout = std::chrono::seconds(timeout_int);
    }

    if (wait_for_file(args[1].c_str(), timeout) != 0) {
        return Error() << "wait_for_file() failed";
    }

    return {};
}

static Result<void> do_wait_for_prop(const BuiltinArguments& args) {
    const char* name = args[1].c_str();
    const char* value = args[2].c_str();
    size_t value_len = strlen(value);

    if (!IsLegalPropertyName(name)) {
        return Error() << "IsLegalPropertyName(" << name << ") failed";
    }
    if (value_len >= PROP_VALUE_MAX) {
        return Error() << "value too long";
    }
    if (!start_waiting_for_property(name, value)) {
        return Error() << "already waiting for a property";
    }
    return {};
}

static bool is_file_crypto() {
    return android::base::GetProperty("ro.crypto.type", "") == "file";
}

static Result<void> ExecWithFunctionOnFailure(const std::vector<std::string>& args,
                                              std::function<void(const std::string&)> function) {
    auto service = Service::MakeTemporaryOneshotService(args);
    if (!service) {
        function("MakeTemporaryOneshotService failed: " + service.error().message());
    }
    (*service)->AddReapCallback([function](const siginfo_t& siginfo) {
        if (siginfo.si_code != CLD_EXITED || siginfo.si_status != 0) {
            function(StringPrintf("Exec service failed, status %d", siginfo.si_status));
        }
    });
    if (auto result = (*service)->ExecStart(); !result) {
        function("ExecStart failed: " + result.error().message());
    }
    ServiceList::GetInstance().AddService(std::move(*service));
    return {};
}

static Result<void> ExecVdcRebootOnFailure(const std::string& vdc_arg) {
    auto reboot_reason = vdc_arg + "_failed";

    auto reboot = [reboot_reason](const std::string& message) {
        // TODO (b/122850122): support this in gsi
        if (fscrypt_is_native() && !android::gsi::IsGsiRunning()) {
            LOG(ERROR) << message << ": Rebooting into recovery, reason: " << reboot_reason;
            if (auto result = reboot_into_recovery(
                        {"--prompt_and_wipe_data", "--reason="s + reboot_reason});
                !result) {
                LOG(FATAL) << "Could not reboot into recovery: " << result.error();
            }
        } else {
            LOG(ERROR) << "Failure (reboot suppressed): " << reboot_reason;
        }
    };

    std::vector<std::string> args = {"exec", "/system/bin/vdc", "--wait", "cryptfs", vdc_arg};
    return ExecWithFunctionOnFailure(args, reboot);
}

static Result<void> do_remount_userdata(const BuiltinArguments& args) {
    if (initial_mount_fstab_return_code == -1) {
        return Error() << "Calling remount_userdata too early";
    }
    Fstab fstab;
    if (!ReadDefaultFstab(&fstab)) {
        // TODO(b/135984674): should we reboot here?
        return Error() << "Failed to read fstab";
    }
    // TODO(b/135984674): check that fstab contains /data.
    if (auto rc = fs_mgr_remount_userdata_into_checkpointing(&fstab); rc < 0) {
        TriggerShutdown("reboot,mount-userdata-failed");
    }
    if (auto result = queue_fs_event(initial_mount_fstab_return_code, true); !result) {
        return Error() << "queue_fs_event() failed: " << result.error();
    }
    return {};
}

static Result<void> do_installkey(const BuiltinArguments& args) {
    if (!is_file_crypto()) return {};

    auto unencrypted_dir = args[1] + fscrypt_unencrypted_folder;
    if (!make_dir(unencrypted_dir, 0700) && errno != EEXIST) {
        return ErrnoError() << "Failed to create " << unencrypted_dir;
    }
    return ExecVdcRebootOnFailure("enablefilecrypto");
}

static Result<void> do_init_user0(const BuiltinArguments& args) {
    return ExecVdcRebootOnFailure("init_user0");
}

static Result<void> do_mark_post_data(const BuiltinArguments& args) {
    ServiceList::GetInstance().MarkPostData();

    return {};
}

static Result<void> do_parse_apex_configs(const BuiltinArguments& args) {
    glob_t glob_result;
    static constexpr char glob_pattern[] = "/apex/*/etc/*.rc";
    const int ret = glob(glob_pattern, GLOB_MARK, nullptr, &glob_result);
    if (ret != 0 && ret != GLOB_NOMATCH) {
        globfree(&glob_result);
        return Error() << "glob pattern '" << glob_pattern << "' failed";
    }
    std::vector<std::string> configs;
    Parser parser = CreateServiceOnlyParser(ServiceList::GetInstance());
    for (size_t i = 0; i < glob_result.gl_pathc; i++) {
        std::string path = glob_result.gl_pathv[i];
        // Filter-out /apex/<name>@<ver> paths. The paths are bind-mounted to
        // /apex/<name> paths, so unless we filter them out, we will parse the
        // same file twice.
        std::vector<std::string> paths = android::base::Split(path, "/");
        if (paths.size() >= 3 && paths[2].find('@') != std::string::npos) {
            continue;
        }
        configs.push_back(path);
    }
    globfree(&glob_result);

    bool success = true;
    for (const auto& c : configs) {
        if (c.back() == '/') {
            // skip if directory
            continue;
        }
        success &= parser.ParseConfigFile(c);
    }
    ServiceList::GetInstance().MarkServicesUpdate();
    if (success) {
        return {};
    } else {
        return Error() << "Could not parse apex configs";
    }
}

static Result<void> do_enter_default_mount_ns(const BuiltinArguments& args) {
    if (SwitchToDefaultMountNamespace()) {
        return {};
    } else {
        return Error() << "Failed to enter into default mount namespace";
    }
}

// Builtin-function-map start
const BuiltinFunctionMap& GetBuiltinFunctionMap() {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    // clang-format off
    static const BuiltinFunctionMap builtin_functions = {
        {"bootchart",               {1,     1,    {false,  do_bootchart}}},
        {"chmod",                   {2,     2,    {true,   do_chmod}}},
        {"chown",                   {2,     3,    {true,   do_chown}}},
        {"class_reset",             {1,     1,    {false,  do_class_reset}}},
        {"class_reset_post_data",   {1,     1,    {false,  do_class_reset_post_data}}},
        {"class_restart",           {1,     1,    {false,  do_class_restart}}},
        {"class_start",             {1,     1,    {false,  do_class_start}}},
        {"class_start_post_data",   {1,     1,    {false,  do_class_start_post_data}}},
        {"class_stop",              {1,     1,    {false,  do_class_stop}}},
        {"copy",                    {2,     2,    {true,   do_copy}}},
        {"domainname",              {1,     1,    {true,   do_domainname}}},
        {"enable",                  {1,     1,    {false,  do_enable}}},
        {"exec",                    {1,     kMax, {false,  do_exec}}},
        {"exec_background",         {1,     kMax, {false,  do_exec_background}}},
        {"exec_start",              {1,     1,    {false,  do_exec_start}}},
        {"export",                  {2,     2,    {false,  do_export}}},
        {"hostname",                {1,     1,    {true,   do_hostname}}},
        {"ifup",                    {1,     1,    {true,   do_ifup}}},
        {"init_user0",              {0,     0,    {false,  do_init_user0}}},
        {"insmod",                  {1,     kMax, {true,   do_insmod}}},
        {"installkey",              {1,     1,    {false,  do_installkey}}},
        {"interface_restart",       {1,     1,    {false,  do_interface_restart}}},
        {"interface_start",         {1,     1,    {false,  do_interface_start}}},
        {"interface_stop",          {1,     1,    {false,  do_interface_stop}}},
        {"load_persist_props",      {0,     0,    {false,  do_load_persist_props}}},
        {"load_system_props",       {0,     0,    {false,  do_load_system_props}}},
        {"loglevel",                {1,     1,    {false,  do_loglevel}}},
        {"mark_post_data",          {0,     0,    {false,  do_mark_post_data}}},
        {"mkdir",                   {1,     6,    {true,   do_mkdir}}},
        // TODO: Do mount operations in vendor_init.
        // mount_all is currently too complex to run in vendor_init as it queues action triggers,
        // imports rc scripts, etc.  It should be simplified and run in vendor_init context.
        // mount and umount are run in the same context as mount_all for symmetry.
        {"mount_all",               {1,     kMax, {false,  do_mount_all}}},
        {"mount",                   {3,     kMax, {false,  do_mount}}},
        {"parse_apex_configs",      {0,     0,    {false,  do_parse_apex_configs}}},
        {"umount",                  {1,     1,    {false,  do_umount}}},
        {"umount_all",              {1,     1,    {false,  do_umount_all}}},
        {"readahead",               {1,     2,    {true,   do_readahead}}},
        {"remount_userdata",        {0,     0,    {false,  do_remount_userdata}}},
        {"restart",                 {1,     1,    {false,  do_restart}}},
        {"restorecon",              {1,     kMax, {true,   do_restorecon}}},
        {"restorecon_recursive",    {1,     kMax, {true,   do_restorecon_recursive}}},
        {"rm",                      {1,     1,    {true,   do_rm}}},
        {"rmdir",                   {1,     1,    {true,   do_rmdir}}},
        {"setprop",                 {2,     2,    {true,   do_setprop}}},
        {"setrlimit",               {3,     3,    {false,  do_setrlimit}}},
        {"start",                   {1,     1,    {false,  do_start}}},
        {"stop",                    {1,     1,    {false,  do_stop}}},
        {"swapon_all",              {1,     1,    {false,  do_swapon_all}}},
        {"enter_default_mount_ns",  {0,     0,    {false,  do_enter_default_mount_ns}}},
        {"symlink",                 {2,     2,    {true,   do_symlink}}},
        {"sysclktz",                {1,     1,    {false,  do_sysclktz}}},
        {"trigger",                 {1,     1,    {false,  do_trigger}}},
        {"verity_update_state",     {0,     0,    {false,  do_verity_update_state}}},
        {"wait",                    {1,     2,    {true,   do_wait}}},
        {"wait_for_prop",           {2,     2,    {false,  do_wait_for_prop}}},
        {"write",                   {2,     2,    {true,   do_write}}},
    };
    // clang-format on
    return builtin_functions;
}
// Builtin-function-map end

}  // namespace init
}  // namespace android
