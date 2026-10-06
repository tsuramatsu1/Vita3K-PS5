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
// libc functions the console does not export but the SDK's headers declare, so dependencies' configure checks find
// them. The title link binds each libc name to the ps5_ function here (cmake/ps5-title.cmake), as the payload SDK
// fork's platform layer does for its own

#include <cerrno>
#include <climits>

#include <netdb.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {

int ps5_mkstemps(char *path_template, int suffix_length);

// The advice is optional: ignoring it is a valid implementation
int ps5_posix_fadvise(int, off_t, off_t, int) {
    return 0;
}

// A title runs with no saved set-user-ID, so the real, effective and saved IDs come from the calls the kernel exports
int ps5_getresuid(uid_t *real, uid_t *effective, uid_t *saved) {
    *real = getuid();
    *effective = geteuid();
    *saved = *effective;
    return 0;
}

int ps5_getresgid(gid_t *real, gid_t *effective, gid_t *saved) {
    *real = getgid();
    *effective = getegid();
    *saved = *effective;
    return 0;
}

// No module a title loads exports these: libScePosixForWebKit's and libkernel_sys's exports are not loaded, so an
// import that only their stubs define is null at run time, and the first call jumps to address 0 (PS5_RetroArch
// tools/build.sh records the same)

int ps5_isatty(int) {
    errno = ENOTTY;
    return 0;
}

int ps5_mkstemp(char *path_template) {
    return ps5_mkstemps(path_template, 0);
}

// The console's FreeBSD limits, for a path that exists
long ps5_pathconf(const char *path, int name) {
    struct stat status;
    if (!path || stat(path, &status) != 0)
        return -1;
    switch (name) {
    case _PC_PATH_MAX:
        return PATH_MAX;
    case _PC_NAME_MAX:
        return NAME_MAX;
    default:
        errno = EINVAL;
        return -1;
    }
}

// The title's file systems have no links
ssize_t ps5_readlink(const char *, char *, size_t) {
    errno = EINVAL;
    return -1;
}

int ps5_link(const char *, const char *) {
    errno = EPERM;
    return -1;
}

int ps5_symlink(const char *, const char *) {
    errno = EPERM;
    return -1;
}

// A title cannot make processes or sessions
int ps5_setsid() {
    errno = EPERM;
    return -1;
}

pid_t ps5_vfork() {
    errno = ENOSYS;
    return -1;
}

// Reverse lookups and the old resolver are unavailable; getaddrinfo is the platform layer's
int ps5_getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int) {
    return EAI_FAIL;
}

struct hostent *ps5_gethostbyname(const char *) {
    return nullptr;
}

const char *ps5_gai_strerror(int) {
    return "name resolution is unavailable";
}
}
