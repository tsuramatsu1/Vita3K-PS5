// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <platform/platform.h>

#include <util/log.h>

#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int sceKernelUsleep(unsigned int microseconds);
int sceSystemServicePowerTick();
int sceSystemServiceHideSplashScreen();
int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);

// The title crt calls this with main's result, then exit(), which the console answers with SIGSYS, a crash report
// and a forced kill. Asking the shell to end the title instead returns to the home screen cleanly
void catchReturnFromMain(int status) {
    // The log is written on its own thread, so whatever is still queued when a title leaves would be lost - and
    // that tail is exactly what says why it left
    std::fprintf(stderr, "leaving: main returned %d\n", status);
    logging::flush();
    std::fflush(nullptr);
    sceSystemServiceLoadExec("exit", nullptr);
    for (;;)
        sceKernelUsleep(100000);
}
}

namespace platform {
namespace {

// A title runs in a sandbox that sees only its own mounts. The PS5SX2 Helper frees the process that publishes its
// PID here, if the title ID is on its allowlist (/data/whitelist.txt). The request keeps the file name of the
// payload the Helper is built on, which is what it still watches for
constexpr const char *JAILBREAK_REQUEST = "/download0/etahen_jailbreak";
constexpr const char *JAILBREAK_REQUEST_STAGED = "/download0/etahen_jailbreak.tmp";
constexpr unsigned int JAILBREAK_POLL_US = 16667;
// The payload consumes the request within seconds, then needs a moment more to finish
constexpr int JAILBREAK_CONSUME_POLLS = 600;
constexpr int JAILBREAK_SETTLE_POLLS = 450;

constexpr const char *DATA_DIRECTORY = "/data/Vita3K";
// The title's own storage, which the sandbox always allows
constexpr const char *SANDBOX_DIRECTORY = "/download0/Vita3K";

bool publish_jailbreak_request() {
    unlink(JAILBREAK_REQUEST_STAGED);
    const int fd = open(JAILBREAK_REQUEST_STAGED, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "jailbreak: cannot create %s: %s\n", JAILBREAK_REQUEST_STAGED, std::strerror(errno));
        return false;
    }

    const std::string request = "{\"PID\":" + std::to_string(getpid()) + "}\n";
    const bool written = fchmod(fd, 0666) == 0
        && write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size())
        && fsync(fd) == 0;
    close(fd);
    if (!written || rename(JAILBREAK_REQUEST_STAGED, JAILBREAK_REQUEST) != 0) {
        std::fprintf(stderr, "jailbreak: cannot publish the request: %s\n", std::strerror(errno));
        unlink(JAILBREAK_REQUEST_STAGED);
        return false;
    }
    return true;
}

// Whether the process can now reach the console's file system outside the sandbox
bool data_reachable() {
    return access("/data", W_OK) == 0;
}

bool request_jailbreak() {
    if (data_reachable())
        return true;
    if (!publish_jailbreak_request())
        return false;

    int polls = 0;
    while (access(JAILBREAK_REQUEST, F_OK) == 0) {
        if (++polls >= JAILBREAK_CONSUME_POLLS) {
            std::fprintf(stderr, "jailbreak: the PS5SX2 Helper did not answer; is it loaded, and is PPSA99300 in /data/whitelist.txt?\n");
            unlink(JAILBREAK_REQUEST);
            return false;
        }
        sceKernelUsleep(JAILBREAK_POLL_US);
    }
    for (polls = 0; polls < JAILBREAK_SETTLE_POLLS; polls++) {
        if (data_reachable())
            return true;
        sceKernelUsleep(JAILBREAK_POLL_US);
    }
    std::fprintf(stderr, "jailbreak: the request was consumed, but /data is still out of reach\n");
    return false;
}

// The memory the title has, and whether malloc is served by the direct-memory heap (a dlmalloc clash once sent
// every allocation to libc's small heap, which failed startup's first large one)
void report_memory() {
    int64_t direct_start = 0;
    size_t direct_available = 0;
    sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), PS5_KERNEL_DIRECT_ALIGNMENT, &direct_start, &direct_available);
    size_t flexible_available = 0;
    sceKernelAvailableFlexibleMemorySize(&flexible_available);
    struct ps5_heap_stats stats{};
    ::ps5_heap_stats(&stats);
    std::fprintf(stderr, "memory: direct %zu MiB free of %lld MiB, flexible %zu MiB free; heap %zu MiB mapped, %llu allocations served by libc\n",
        direct_available >> 20, static_cast<long long>(sceKernelGetDirectMemorySize() >> 20), flexible_available >> 20,
        stats.mapped_bytes >> 20, stats.libc_fallbacks);
}

// What of the console's file system this process can reach. A title sees only its own sandbox until the jailbreak
// widens that, and the USB drives are a separate question from /data
void probe_mounts() {
    // What the title's own root holds, which says whether it is the console's root or still the sandbox
    if (DIR *listing = opendir("/")) {
        std::string names;
        while (const dirent *entry = readdir(listing)) {
            if (entry->d_name[0] == '.')
                continue;
            if (!names.empty())
                names += " ";
            names += entry->d_name;
        }
        closedir(listing);
        std::fprintf(stderr, "mounts: / holds %s\n", names.c_str());
    } else {
        std::fprintf(stderr, "mounts: / cannot be listed (%s)\n", std::strerror(errno));
    }

    for (const char *path : { "/data", "/mnt", "/mnt/usb0", "/usb0", "/host", "/preinst", "/user", "/download0" }) {
        struct stat info = {};
        if (stat(path, &info) != 0) {
            std::fprintf(stderr, "mounts: %s cannot be read (%s)\n", path, std::strerror(errno));
            continue;
        }
        int entries = 0;
        if (DIR *listing = opendir(path)) {
            while (readdir(listing))
                entries++;
            closedir(listing);
            std::fprintf(stderr, "mounts: %s is a folder with %d entries\n", path, entries);
        } else {
            std::fprintf(stderr, "mounts: %s cannot be listed (%s)\n", path, std::strerror(errno));
        }
    }
}

// Removals are refused under some of what the title can reach and allowed under the rest. Each path is reported
// with the device it sits on, so a subtree that is really a separate mount shows up as a different number
void probe_removal_by_subtree(const std::string &save_directory) {
    const std::string places[] = { "/data", save_directory, save_directory + "/vita", save_directory + "/vita/ux0",
        save_directory + "/vita/ux0/app" };
    for (const std::string &place : places) {
        struct stat info = {};
        if (stat(place.c_str(), &info) != 0) {
            std::fprintf(stderr, "removal: %s cannot be read (%s)\n", place.c_str(), std::strerror(errno));
            continue;
        }
        const std::string probe = place + "/.vita3k-removal-probe";
        const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            std::fprintf(stderr, "removal: %s device=%ju uid=%u mode=%o, cannot write there (%s)\n", place.c_str(),
                static_cast<std::uintmax_t>(info.st_dev), info.st_uid, info.st_mode & 07777, std::strerror(errno));
            continue;
        }
        close(fd);
        const int result = unlink(probe.c_str());
        std::fprintf(stderr, "removal: %s device=%ju uid=%u mode=%o, unlink=%d (%s)\n", place.c_str(),
            static_cast<std::uintmax_t>(info.st_dev), info.st_uid, info.st_mode & 07777, result,
            result == 0 ? "allowed" : std::strerror(errno));
    }
}

// What this storage will actually take, independent of anything the installer does. An install writes a game's
// worth of data and slows as it goes, so this writes a run of it the same way and reports the rate for each
// quarter: a steady figure means the installer is at fault, a falling one means the storage is
void probe_one_storage(const std::string &directory, std::size_t chunk, const char *label) {
    constexpr std::size_t TOTAL = 64u << 20;
    const std::string path = directory + "/.write-probe";
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "storage: %s cannot be written (%s)\n", directory.c_str(), std::strerror(errno));
        return;
    }
    std::vector<std::uint8_t> block(chunk, 0xa5);
    const auto now = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const double started = now();
    bool ok = true;
    for (std::size_t written = 0; written < TOTAL && ok; written += chunk)
        ok = ::write(fd, block.data(), chunk) == static_cast<ssize_t>(chunk);
    const double spent = now() - started;
    close(fd);
    unlink(path.c_str());
    if (ok)
        std::fprintf(stderr, "storage: %s in %s blocks: %.1f MB/s\n", label, chunk >= (1u << 20) ? "2 MiB" : "64 KiB",
            spent > 0 ? (TOTAL / 1e6) / spent : 0.0);
    else
        std::fprintf(stderr, "storage: %s write failed (%s)\n", label, std::strerror(errno));
}

// Where the slowness lives. The same bytes are written to the storage the jailbreak grants and to the title's own
// sandbox, in two block sizes: a payload process reaches about 90 MB/s on the first of these, so a title that does
// not says the cost is in how this process reaches it rather than in the device
void probe_storage_speed(const std::string &directory) {
    probe_one_storage(directory, 2u << 20, "granted storage");
    probe_one_storage(directory, 64u << 10, "granted storage");
    probe_one_storage("/download0", 2u << 20, "own sandbox");
    probe_one_storage("/download0", 64u << 10, "own sandbox");
}


// Whether this process may delete what it creates: installing firmware deletes its temporary files, and the console
// refused that once (EPERM). Each case is logged with its errno
void probe_file_removal(const std::string &directory) {
    const std::string folder = directory + "/.removal-probe";
    const std::string file = folder + "/file";
    mkdir(folder.c_str(), 0777);

    int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, "x", 1);
        close(fd);
    }
    const int closed_result = unlink(file.c_str());
    const int closed_errno = errno;

    fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
        close(fd);
    DIR *listing = opendir(folder.c_str());
    const int listed_result = unlink(file.c_str());
    const int listed_errno = errno;
    if (listing)
        closedir(listing);
    unlink(file.c_str());

    // The firmware installer's case: nested folders, a package segment's name, written through a C++ stream
    const std::string nested = folder + "/PUP_dec";
    mkdir(nested.c_str(), 0777);
    for (const char *name : { "sa0-00.pkg.seg02", "plain.bin" }) {
        const std::string path = nested + "/" + name;
        std::FILE *stream = std::fopen(path.c_str(), "wb");
        if (stream) {
            std::fputs("x", stream);
            std::fclose(stream);
        }
        const int result = unlink(path.c_str());
        std::fprintf(stderr, "removal probe: unlink %s=%d (%d)\n", path.c_str(), result, result ? errno : 0);
    }
    const int nested_result = rmdir(nested.c_str());
    std::fprintf(stderr, "removal probe: rmdir %s=%d (%d)\n", nested.c_str(), nested_result, nested_result ? errno : 0);

    const int rmdir_result = rmdir(folder.c_str());
    const int rmdir_errno = errno;
    std::fprintf(stderr, "removal probe in %s: unlink closed file=%d (%d), unlink with folder listed=%d (%d, listing %s), rmdir=%d (%d), euid=%d\n",
        directory.c_str(), closed_result, closed_result ? closed_errno : 0, listed_result, listed_result ? listed_errno : 0,
        listing ? "open" : "failed", rmdir_result, rmdir_result ? rmdir_errno : 0, static_cast<int>(geteuid()));
}

class Ps5Platform final : public PlatformInterface {
public:
    RuntimeConfig config() const override {
        return RuntimeConfig{
            .kind = PlatformKind::PS5,
            .app_name = "Vita3K",
            .static_assets_directory = "/app0",
            .save_directory = save_directory,
        };
    }

    bool initialize() override {
        // Nothing reads a title's standard error on the console, so send it to klog
        const bool klog = ps5_klog_capture_stderr("[Vita3K] ") == 0;

        report_memory();
        if (request_jailbreak()) {
            save_directory = DATA_DIRECTORY;
        } else {
            save_directory = SANDBOX_DIRECTORY;
            std::fprintf(stderr, "running sandboxed: files go to %s, which other apps cannot reach\n", SANDBOX_DIRECTORY);
        }
        std::fprintf(stderr, "save directory: %s\n", save_directory.c_str());
        probe_file_removal(save_directory);
        probe_removal_by_subtree(save_directory);
        probe_mounts();
        probe_storage_speed(save_directory);
        return klog;
    }

    void shutdown() override {
    }

private:
    std::string save_directory = SANDBOX_DIRECTORY;
};

} // namespace

// The shell covers everything the title presents with its splash until told otherwise, so this comes with the first
// frame: the splash stays up through start-up instead of a black screen
void hide_splash_screen() {
    static bool hidden = false;
    if (!hidden) {
        hidden = true;
        sceSystemServiceHideSplashScreen();
    }
}

// The console counts the time since the last button press and lowers its power state after a minute of none. An
// install takes no input at all, so without this its decryption slows by more than ten times partway through
void keep_awake() {
    constexpr std::int64_t EVERY_US = 20'000'000;
    static std::int64_t last = 0;
    const std::int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    if (now - last < EVERY_US)
        return;
    last = now;
    sceSystemServicePowerTick();
}

// Reports what stands in the way of removing a path: EPERM on a file this process owns, inside a folder it can
// write, points at a flag set on the file (chflags) rather than a missing permission
void report_undeletable(const std::string &path) {
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0) {
        std::fprintf(stderr, "undeletable: %s cannot be read (%s)\n", path.c_str(), std::strerror(errno));
        return;
    }
    std::fprintf(stderr, "undeletable: %s %s uid=%u gid=%u mode=%o flags=%#x euid=%u\n", path.c_str(),
        S_ISDIR(info.st_mode) ? "folder" : "file", info.st_uid, info.st_gid, info.st_mode & 07777,
        static_cast<unsigned>(info.st_flags), geteuid());

    if (!S_ISDIR(info.st_mode)) {
        const int result = unlink(path.c_str());
        std::fprintf(stderr, "undeletable: unlink = %d (%s)\n", result, result == 0 ? "allowed" : std::strerror(errno));
        return;
    }

    // The first few entries, each with its own flags, and the first file actually tried
    DIR *listing = opendir(path.c_str());
    if (!listing) {
        std::fprintf(stderr, "undeletable: cannot list it (%s)\n", std::strerror(errno));
        return;
    }
    int shown = 0;
    bool tried = false;
    while (const dirent *entry = readdir(listing)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string child = path + "/" + name;
        struct stat child_info = {};
        if (stat(child.c_str(), &child_info) != 0)
            continue;
        if (shown++ < 4)
            std::fprintf(stderr, "undeletable:   %s %s uid=%u mode=%o flags=%#x\n", name.c_str(),
                S_ISDIR(child_info.st_mode) ? "folder" : "file", child_info.st_uid, child_info.st_mode & 07777,
                static_cast<unsigned>(child_info.st_flags));
        if (!tried && !S_ISDIR(child_info.st_mode)) {
            tried = true;
            const int result = unlink(child.c_str());
            std::fprintf(stderr, "undeletable:   unlink %s = %d (%s)\n", name.c_str(), result,
                result == 0 ? "allowed" : std::strerror(errno));
        }
    }
    closedir(listing);
    const int result = rmdir(path.c_str());
    std::fprintf(stderr, "undeletable: rmdir = %d (%s)\n", result, result == 0 ? "allowed" : std::strerror(errno));
}

// Removes a folder and all it holds, walking it with plain unlink and rmdir. The standard library's remove_all
// decides what each entry is before removing it, and where that decision comes out wrong it unlinks a folder,
// which FreeBSD refuses with the same "operation not permitted" a protected file would give. Both calls used here
// are known to work on the console. Returns what it could not remove, so nothing reports success it did not have
int remove_tree(const std::string &path) {
    struct stat info = {};
    // stat, not lstat: the console's lstat is not the one to rely on here, and these trees hold no links
    if (stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT)
            return 0;
        std::fprintf(stderr, "remove: cannot read %s (%s)\n", path.c_str(), std::strerror(errno));
        return 1;
    }

    if (!S_ISDIR(info.st_mode)) {
        if (unlink(path.c_str()) == 0)
            return 0;
        std::fprintf(stderr, "remove: cannot unlink %s (%s)\n", path.c_str(), std::strerror(errno));
        return 1;
    }

    // The names first, then the removals: deleting while the folder is open can skip entries
    std::vector<std::string> names;
    DIR *listing = opendir(path.c_str());
    if (!listing) {
        std::fprintf(stderr, "remove: cannot list %s (%s)\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    while (const dirent *entry = readdir(listing)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..")
            names.push_back(name);
    }
    closedir(listing);

    int left = 0;
    for (const std::string &name : names)
        left += remove_tree(path + "/" + name);

    if (left > 0)
        return left;
    if (rmdir(path.c_str()) != 0) {
        std::fprintf(stderr, "remove: cannot remove folder %s (%s)\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    return 0;
}

// The kernel's toast: a 0xc30-byte request whose text starts at byte 0x2d (PS5SX2's ProsperoNotify.cpp)
void notify(const std::string &message) {
    constexpr size_t REQUEST_SIZE = 0xc30;
    constexpr size_t TEXT_OFFSET = 0x2d;
    constexpr size_t TEXT_SIZE = 1024;
    char request[REQUEST_SIZE] = {};
    std::strncpy(request + TEXT_OFFSET, message.c_str(), TEXT_SIZE - 1);
    sceKernelSendNotificationRequest(0, request, REQUEST_SIZE, 0);
}

std::unique_ptr<PlatformInterface> create_platform() {
    return std::make_unique<Ps5Platform>();
}

} // namespace platform
