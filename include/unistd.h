/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#pragma once

#ifdef _WIN32

#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

// ---- Trivial stubs ----

inline int fchmod(int, mode_t)       { errno = ENOSYS; return -1; }
inline int fchown(int, uid_t, gid_t) { errno = ENOSYS; return -1; }
inline int fdatasync(int fd)         { return _commit(fd); }
inline int symlink(const char* tgt, const char* lnk) {
    return CreateSymbolicLinkA(lnk, tgt, 0) ? 0 : -1;
}
inline int link(const char* oldname, const char* newname) {
    return CreateHardLinkA(newname, oldname, NULL) ? 0 : -1;
}
inline int chown(const char*, uid_t, gid_t)   { errno = ENOSYS; return -1; }
inline int lchown(const char*, uid_t, gid_t)  { errno = ENOSYS; return -1; }
inline int lstat(const char* path, struct stat* buf) { return stat(path, buf); }
inline unsigned int sleep(unsigned int seconds) { Sleep(seconds * 1000); return 0; }
inline void sync(void) {}
inline int ftruncate(int fd, off_t length) { return _chsize_s(fd, length); }
inline int utimes(const char*, const struct timeval*) { errno = ENOSYS; return -1; }
inline int lutimes(const char*, const struct timeval*) { errno = ENOSYS; return -1; }
inline int mknod(const char*, mode_t, dev_t) { errno = ENOSYS; return -1; }

// ---- Non-trivial: implemented in common/win32_compat.cpp ----

int fsync(int fd);
ssize_t pread(int fd, void* buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset);
ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset);
ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset);
ssize_t readlink(const char* path, char* buf, size_t bufsiz);
int truncate(const char* path, off_t length);

#else
#include_next <unistd.h>
#endif
