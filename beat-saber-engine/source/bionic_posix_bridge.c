/* Filesystem and process metadata used by the original Quest AArch64 ELF.
 * Forward only operations which libnx's real fsdev can perform. GameActivity
 * pipe descriptors are never passed into newlib's descriptor table.
 */
#include "bionic_posix_bridge.h"
#include "asset_fs_bridge.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#define BS_VIRTUAL_FD_BASE 0x4000
#define BS_REALPATH_CAPACITY 4096
#define BS_DIR_FD_TRACK_CAPACITY 64
/* Android/Linux constants; native newlib AT_* and LOCK_* can differ. */
#define BS_ANDROID_AT_FDCWD (-100)
#define BS_ANDROID_AT_SYMLINK_NOFOLLOW 0x100
#define BS_ANDROID_LOCK_SH 1
#define BS_ANDROID_LOCK_EX 2
#define BS_ANDROID_LOCK_NB 4
#define BS_ANDROID_LOCK_UN 8
#define BS_ANDROID_UTIME_OMIT 1073741822LL
#define BS_ANDROID_UTIME_NOW 1073741823LL

_Static_assert(sizeof(BsBionicStatvfs) == 112, "Android AArch64 statvfs ABI");
_Static_assert(sizeof(BsBionicUtsname) == 390, "Android AArch64 utsname ABI");
_Static_assert(sizeof(BsBionicTimespec) == 16, "Android AArch64 timespec ABI");

#ifdef __SWITCH__
int bs_bionic_poll(BsBionicPollfd *fds, uint32_t count, int timeout_ms) {
    uint64_t limit = timeout_ms < 0 ? UINT64_MAX : (uint64_t)timeout_ms * 1000000;
    return bs_poll_wait(fds, count, limit);
}

int bs_bionic_select(int nfds, uint64_t *read_fds, uint64_t *write_fds,
                     uint64_t *except_fds, BsBionicTimeval *timeout) {
    enum { BS_POLLIN = 1, BS_POLLPRI = 2, BS_POLLOUT = 4, BS_POLLERR = 8,
           BS_POLLHUP = 16, BS_POLLNVAL = 32 };
    if (nfds < 0 || nfds > 1024 ||
        (timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
                     timeout->tv_usec >= 1000000))) {
        errno = EINVAL;
        return -1;
    }
    uint64_t limit = UINT64_MAX;
    if (timeout) {
        uint64_t fractional_ns = (uint64_t)timeout->tv_usec * 1000;
        uint64_t seconds = (uint64_t)timeout->tv_sec;
        if (seconds <= (UINT64_MAX - 1 - fractional_ns) / 1000000000)
            limit = seconds * 1000000000 + fractional_ns;
        else
            limit = UINT64_MAX - 1;
    }
    BsBionicPollfd *fds = nfds ? malloc((size_t)nfds * sizeof(*fds)) : NULL;
    if (nfds && !fds) return -1;
    uint64_t *const sets[] = {read_fds, write_fds, except_fds};
    const int interests[] = {BS_POLLIN, BS_POLLOUT, BS_POLLPRI};
    uint32_t count = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        uint64_t bit = UINT64_C(1) << (fd % 64);
        int events = 0;
        for (size_t k = 0; k < 3; ++k)
            if (sets[k] && (sets[k][fd / 64] & bit)) events |= interests[k];
        if (events) fds[count++] = (BsBionicPollfd){fd, events, 0};
    }
    u64 start = armGetSystemTick();
    int result = bs_poll_wait(fds, count, limit);
    if (result < 0) { free(fds); return -1; }
    for (uint32_t k = 0; k < count; ++k) {
        if (fds[k].revents & BS_POLLNVAL) {
            free(fds);
            errno = EBADF;
            return -1;
        }
    }
    result = 0;
    size_t bytes = ((size_t)nfds + 63) / 64 * sizeof(uint64_t);
    for (size_t k = 0; k < 3; ++k)
        if (sets[k]) memset(sets[k], 0, bytes);
    const int reports[] = {BS_POLLIN | BS_POLLERR | BS_POLLHUP,
                           BS_POLLOUT | BS_POLLERR, BS_POLLPRI};
    for (uint32_t i = 0; i < count; ++i) {
        int fd = fds[i].fd;
        int ready = 0;
        for (size_t k = 0; k < 3; ++k) {
            if (!(fds[i].events & interests[k]) || !(fds[i].revents & reports[k]))
                continue;
            sets[k][fd / 64] |= UINT64_C(1) << (fd % 64);
            ready = 1;
        }
        result += ready;
    }
    free(fds);
    if (timeout) {
        uint64_t elapsed = armTicksToNs(armGetSystemTick() - start);
        uint64_t remaining = limit > elapsed ? limit - elapsed : 0;
        timeout->tv_sec = (int64_t)(remaining / 1000000000);
        timeout->tv_usec = (int64_t)((remaining % 1000000000) / 1000);
    }
    return result;
}
#endif

static int bs_posix_fd_valid(int fd) {
    if (fd < 0 || fd >= BS_VIRTUAL_FD_BASE) {
        errno = EBADF;
        return 0;
    }
    return 1;
}

/* newlib's Switch DIR* cannot adopt an existing native directory descriptor.
 * Keep a bounded, locked record of real directory opens so fdopendir can
 * re-open the SAME normalized native path and consume its originating fd.
 * An fd may be closed by another bridge and reused: fstat identity prevents
 * trusting stale records. All records are best-effort; an untracked directory
 * fails explicitly on Switch instead of returning an unrelated directory. */
typedef struct BsDirectoryFdRecord {
    int fd;
    dev_t device;
    ino_t inode;
    char *path;
} BsDirectoryFdRecord;
static BsDirectoryFdRecord g_dir_fd_records[BS_DIR_FD_TRACK_CAPACITY];
static pthread_mutex_t g_dir_fd_lock = PTHREAD_MUTEX_INITIALIZER;

static int bs_dir_record_matches(const BsDirectoryFdRecord *entry,
                                 int fd, const struct stat *descriptor) {
    return entry->path && entry->fd == fd &&
           S_ISDIR(descriptor->st_mode) &&
           entry->device == descriptor->st_dev &&
           entry->inode == descriptor->st_ino;
}

static void bs_dir_record_clear(BsDirectoryFdRecord *entry) {
    free(entry->path);
    memset(entry, 0, sizeof *entry);
}

static BsDirectoryFdRecord *bs_dir_record_find(int fd) {
    for (size_t i = 0; i < BS_DIR_FD_TRACK_CAPACITY; ++i)
        if (g_dir_fd_records[i].path && g_dir_fd_records[i].fd == fd)
            return &g_dir_fd_records[i];
    return NULL;
}

void bs_bionic_note_open_fd(int fd, const char *native_path) {
    if (fd < 0 || fd >= BS_VIRTUAL_FD_BASE) return;
    struct stat descriptor;
    int is_dir = native_path && fstat(fd, &descriptor) == 0 &&
                 S_ISDIR(descriptor.st_mode);
    char canonical[BS_REALPATH_CAPACITY];
    char *copy = NULL;
    if (is_dir && realpath(native_path, canonical))
        copy = strdup(canonical);
    pthread_mutex_lock(&g_dir_fd_lock);
    BsDirectoryFdRecord *slot = bs_dir_record_find(fd);
    if (slot) bs_dir_record_clear(slot);
    if (copy) {
        if (!slot) {
            for (size_t i = 0; i < BS_DIR_FD_TRACK_CAPACITY; ++i) {
                BsDirectoryFdRecord *candidate = &g_dir_fd_records[i];
                if (!candidate->path) { slot = candidate; break; }
                struct stat current;
                if (fstat(candidate->fd, &current) != 0 ||
                    !bs_dir_record_matches(candidate, candidate->fd, &current)) {
                    bs_dir_record_clear(candidate);
                    slot = candidate;
                    break;
                }
            }
        }
        if (slot) {
            *slot = (BsDirectoryFdRecord){
                .fd = fd, .device = descriptor.st_dev,
                .inode = descriptor.st_ino, .path = copy
            };
            copy = NULL;
        }
    }
    pthread_mutex_unlock(&g_dir_fd_lock);
    free(copy);
}

void bs_bionic_note_dup2_fd(int source_fd, int destination_fd) {
    if (source_fd == destination_fd || destination_fd < 0 ||
        destination_fd >= BS_VIRTUAL_FD_BASE) return;
    char *copy = NULL;
    pthread_mutex_lock(&g_dir_fd_lock);
    BsDirectoryFdRecord *source = bs_dir_record_find(source_fd);
    struct stat descriptor;
    if (source && fstat(source_fd, &descriptor) == 0 &&
        bs_dir_record_matches(source, source_fd, &descriptor))
        copy = strdup(source->path);
    BsDirectoryFdRecord *destination = bs_dir_record_find(destination_fd);
    if (destination) bs_dir_record_clear(destination);
    pthread_mutex_unlock(&g_dir_fd_lock);
    if (copy) {
        bs_bionic_note_open_fd(destination_fd, copy);
        free(copy);
    }
}

/* File descriptors backing Android asset files are native newlib descriptors.
 * Preserve the current file offset: libunity and OVRPlugin use positioned I/O
 * while other code reads the same descriptor concurrently. */
#ifdef __SWITCH__
/* newlib on devkitA64 declares pread/pwrite but does not provide linkable
 * implementations. Serialize positional calls and restore the old offset on
 * every path. Callers sharing an fd with unrelated raw read/write must still
 * coordinate those accesses: the fd table provides no atomic native pread. */
static pthread_mutex_t g_bionic_positional_lock = PTHREAD_MUTEX_INITIALIZER;

static ssize_t bs_bionic_positioned_io(int fd, void *buffer, size_t count,
                                      off_t offset, int writing) {
    int rc = pthread_mutex_lock(&g_bionic_positional_lock);
    if (rc) { errno = rc; return -1; }
    off_t previous = lseek(fd, 0, SEEK_CUR);
    ssize_t result = -1;
    int saved_errno = errno;
    if (previous >= 0 && lseek(fd, offset, SEEK_SET) >= 0) {
        result = writing ? write(fd, buffer, count) : read(fd, buffer, count);
        saved_errno = errno;
        if (lseek(fd, previous, SEEK_SET) < 0 && result >= 0) {
            result = -1;
            saved_errno = errno;
        }
    } else saved_errno = errno;
    pthread_mutex_unlock(&g_bionic_positional_lock);
    errno = saved_errno;
    return result;
}
#endif

static ssize_t bs_bionic_pread_impl(int fd, void *buffer, size_t count, int64_t offset);
ssize_t bs_bionic_pread(int fd, void *buffer, size_t count, int64_t offset) {
    return bs_bionic_pread_impl(fd, buffer, count, offset);
}
static ssize_t bs_bionic_pread_impl(int fd, void *buffer, size_t count, int64_t offset) {
    if (!bs_posix_fd_valid(fd)) return -1;
    if (offset < 0) { errno = EINVAL; return -1; }
    if ((int64_t)(off_t)offset != offset) { errno = EOVERFLOW; return -1; }
    if (!buffer && count) { errno = EFAULT; return -1; }
#ifdef __SWITCH__
    return bs_bionic_positioned_io(fd, buffer, count, (off_t)offset, 0);
#else
    return pread(fd, buffer, count, (off_t)offset);
#endif
}

ssize_t bs_bionic_pwrite(int fd, const void *buffer, size_t count,
                        int64_t offset) {
    if (!bs_posix_fd_valid(fd)) return -1;
    if (offset < 0) { errno = EINVAL; return -1; }
    if ((int64_t)(off_t)offset != offset) { errno = EOVERFLOW; return -1; }
    if (!buffer && count) { errno = EFAULT; return -1; }
#ifdef __SWITCH__
    return bs_bionic_positioned_io(fd, (void *)buffer, count,
                                  (off_t)offset, 1);
#else
    return pwrite(fd, buffer, count, (off_t)offset);
#endif
}

/* fdopendir transfers ownership only after a real native DIR exists. Native
 * fdopendir handles untracked host descriptors; newlib Switch has none. */
void *bs_bionic_fdopendir(int fd) {
    if (!bs_posix_fd_valid(fd)) return NULL;
    struct stat descriptor;
    if (fstat(fd, &descriptor) != 0) return NULL;
    if (!S_ISDIR(descriptor.st_mode)) { errno = ENOTDIR; return NULL; }
    pthread_mutex_lock(&g_dir_fd_lock);
    BsDirectoryFdRecord *record = bs_dir_record_find(fd);
    if (record && !bs_dir_record_matches(record, fd, &descriptor)) {
        bs_dir_record_clear(record);
        record = NULL;
    }
    if (record) {
        struct stat target;
        if (stat(record->path, &target) != 0 || !S_ISDIR(target.st_mode) ||
            target.st_dev != descriptor.st_dev ||
            target.st_ino != descriptor.st_ino) {
            bs_dir_record_clear(record);
            record = NULL;
        }
    }
    if (record) {
        DIR *directory = opendir(record->path);
        if (!directory) {
            int error = errno;
            pthread_mutex_unlock(&g_dir_fd_lock);
            errno = error;
            return NULL;
        }
        if (close(fd) != 0) {
            int error = errno;
            closedir(directory);
            pthread_mutex_unlock(&g_dir_fd_lock);
            errno = error;
            return NULL;
        }
        bs_dir_record_clear(record);
        pthread_mutex_unlock(&g_dir_fd_lock);
        return directory;
    }
    pthread_mutex_unlock(&g_dir_fd_lock);
#ifdef __SWITCH__
    errno = ENOTSUP;
    return NULL;
#else
    return fdopendir(fd);
#endif
}

static const char *bs_posix_native_path(const char *android_path,
                                        char output[BS_REALPATH_CAPACITY]) {
    if (!android_path) { errno = EFAULT; return NULL; }
    /* The existing filesystem bridge owns path mapping and security checks.
     * realpath requires the entry to exist, which all callers here require. */
    return bs_android_realpath(android_path, output);
}

int bs_bionic_ftruncate(int fd, int64_t length) {
    if (!bs_posix_fd_valid(fd)) return -1;
    if (length < 0) { errno = EINVAL; return -1; }
    if ((int64_t)(off_t)length != length) { errno = EFBIG; return -1; }
    return ftruncate(fd, (off_t)length);
}

int bs_bionic_truncate(const char *path, int64_t length) {
    if (!path) { errno = EFAULT; return -1; }
    if (length < 0) { errno = EINVAL; return -1; }
    /* O_WRONLY is Linux/Bionic 1, not a native newlib O_* flag. The asset
     * bridge maps Android private paths and translates open flags. */
    int fd = bs_android_open(path, 1);
    if (fd < 0) return -1;
    int result = bs_bionic_ftruncate(fd, length);
    int previous_error = errno;
    if (close(fd) < 0 && result == 0) return -1;
    if (result < 0) errno = previous_error;
    return result;
}

int bs_bionic_fsync(int fd) {
    if (!bs_posix_fd_valid(fd)) return -1;
    return fsync(fd);
}

int bs_bionic_fdatasync(int fd) {
    /* fsync flushes data AND metadata: at least as strong as fdatasync. */
    return bs_bionic_fsync(fd);
}

int bs_bionic_flock(int fd, int operation) {
    if (!bs_posix_fd_valid(fd)) return -1;
    if ((operation & ~(BS_ANDROID_LOCK_SH | BS_ANDROID_LOCK_EX |
                       BS_ANDROID_LOCK_NB | BS_ANDROID_LOCK_UN)) ||
        ((operation & ~BS_ANDROID_LOCK_NB) != BS_ANDROID_LOCK_SH &&
         (operation & ~BS_ANDROID_LOCK_NB) != BS_ANDROID_LOCK_EX &&
         (operation & ~BS_ANDROID_LOCK_NB) != BS_ANDROID_LOCK_UN)) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    /* No interprocess BSD advisory-lock service exists in libnx fsdev.
     * Even LOCK_UN must not falsely imply a lock was held and released. */
    errno = ENOTSUP;
    return -1;
}

int bs_bionic_statvfs(const char *path, BsBionicStatvfs *output) {
    if (!output) { errno = EFAULT; return -1; }
    char native_path[BS_REALPATH_CAPACITY];
    if (!bs_posix_native_path(path, native_path)) return -1;
    struct statvfs native;
    if (statvfs(native_path, &native) < 0) return -1;
    BsBionicStatvfs converted = {0};
    converted.f_bsize = native.f_bsize;
    converted.f_frsize = native.f_frsize;
    converted.f_blocks = native.f_blocks;
    converted.f_bfree = native.f_bfree;
    converted.f_bavail = native.f_bavail;
    converted.f_files = native.f_files;
    converted.f_ffree = native.f_ffree;
    converted.f_favail = native.f_favail;
    converted.f_fsid = native.f_fsid;
    converted.f_flag = native.f_flag;
    converted.f_namemax = native.f_namemax;
    *output = converted;
    return 0;
}

long bs_bionic_pathconf(const char *path, int name) {
    char native_path[BS_REALPATH_CAPACITY];
    if (!bs_posix_native_path(path, native_path)) return -1;
    /* Android Bionic's _PC_FILESIZEBITS=0, _PC_NAME_MAX=4, etc differ
     * from newlib's _PC_LINK_MAX=0 and _PC_NAME_MAX=3. Indexed by the
     * 20 guest values from AOSP bionic/libc/include/unistd.h. */
    static const int native_names[] = {
        _PC_FILESIZEBITS, _PC_LINK_MAX, _PC_MAX_CANON, _PC_MAX_INPUT,
        _PC_NAME_MAX, _PC_PATH_MAX, _PC_PIPE_BUF, _PC_2_SYMLINKS,
        _PC_ALLOC_SIZE_MIN, _PC_REC_INCR_XFER_SIZE, _PC_REC_MAX_XFER_SIZE,
        _PC_REC_MIN_XFER_SIZE, _PC_REC_XFER_ALIGN, _PC_SYMLINK_MAX,
        _PC_CHOWN_RESTRICTED, _PC_NO_TRUNC, _PC_VDISABLE, _PC_ASYNC_IO,
        _PC_PRIO_IO, _PC_SYNC_IO
    };
    if (name < 0 || (size_t)name >= sizeof(native_names)/sizeof(native_names[0])) {
        errno = EINVAL;
        return -1;
    }
    /* -1/errno=0 means an indeterminate limit and must remain unchanged. */
    return pathconf(native_path, native_names[name]);
}

static int bs_posix_timevals_valid(const BsBionicTimeval times[2]) {
    if (!times) return 1;
    for (int i = 0; i < 2; i++) {
        if (times[i].tv_usec < 0 || times[i].tv_usec >= 1000000) {
            errno = EINVAL;
            return 0;
        }
        if ((int64_t)(time_t)times[i].tv_sec != times[i].tv_sec) {
            errno = EOVERFLOW;
            return 0;
        }
    }
    return 1;
}

int bs_bionic_utimes(const char *path, const BsBionicTimeval times[2]) {
    if (!bs_posix_timevals_valid(times)) return -1;
    char native_path[BS_REALPATH_CAPACITY];
    if (!bs_posix_native_path(path, native_path)) return -1;
#ifdef __SWITCH__
    /* libnx fsdev has no utimes_r implementation and Horizon's IFileSystem
     * only exposes reading timestamps (fsFsGetFileTimeStampRaw). */
    errno = ENOTSUP;
    return -1;
#else
    struct timeval converted[2];
    if (!times) return utimes(native_path, NULL);
    for (int i = 0; i < 2; i++) {
        converted[i].tv_sec = (time_t)times[i].tv_sec;
        converted[i].tv_usec = (suseconds_t)times[i].tv_usec;
    }
    return utimes(native_path, converted);
#endif
}

int bs_bionic_utime(const char *path, const BsBionicUtimbuf *times) {
    if (!times) return bs_bionic_utimes(path, NULL);
    BsBionicTimeval converted[2] = {
        {times->actime, 0}, {times->modtime, 0}
    };
    return bs_bionic_utimes(path, converted);
}

static int bs_posix_timespecs_valid(const BsBionicTimespec times[2]) {
    if (!times) return 1;
    for (int i = 0; i < 2; i++) {
        const int64_t nsec = times[i].tv_nsec;
        if ((nsec < 0 || nsec > 999999999) &&
            nsec != BS_ANDROID_UTIME_NOW && nsec != BS_ANDROID_UTIME_OMIT) {
            errno = EINVAL;
            return 0;
        }
        if (nsec != BS_ANDROID_UTIME_NOW && nsec != BS_ANDROID_UTIME_OMIT &&
            (int64_t)(time_t)times[i].tv_sec != times[i].tv_sec) {
            errno = EOVERFLOW;
            return 0;
        }
    }
    return 1;
}

int bs_bionic_futimens(int fd, const BsBionicTimespec times[2]) {
    if (!bs_posix_fd_valid(fd)) return -1;
    if (!bs_posix_timespecs_valid(times)) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
#ifdef __SWITCH__
    errno = ENOTSUP;
    return -1;
#else
    struct timespec native[2];
    if (!times) return futimens(fd, NULL);
    for (int i = 0; i < 2; i++) {
        native[i].tv_sec = (time_t)times[i].tv_sec;
        native[i].tv_nsec = (long)times[i].tv_nsec;
    }
    return futimens(fd, native);
#endif
}

int bs_bionic_utimensat(int dirfd, const char *path,
                       const BsBionicTimespec times[2], int flags) {
    if (!path) { errno = EFAULT; return -1; }
    if (!bs_posix_timespecs_valid(times)) return -1;
    if (flags & ~BS_ANDROID_AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    if ((path[0] != '/' && strncmp(path, "sdmc:", 5) != 0 &&
         dirfd != BS_ANDROID_AT_FDCWD) ||
        (flags & BS_ANDROID_AT_SYMLINK_NOFOLLOW)) {
        errno = ENOTSUP;
        return -1;
    }
    char native_path[BS_REALPATH_CAPACITY];
    if (!bs_posix_native_path(path, native_path)) return -1;
#ifdef __SWITCH__
    errno = ENOTSUP;
    return -1;
#else
    struct timespec native[2];
    if (!times) return utimensat(AT_FDCWD, native_path, NULL, 0);
    for (int i = 0; i < 2; i++) {
        native[i].tv_sec = (time_t)times[i].tv_sec;
        native[i].tv_nsec = (long)times[i].tv_nsec;
    }
    return utimensat(AT_FDCWD, native_path, native, 0);
#endif
}

/* Horizon account IDs are 128-bit AccountUid values, not POSIX uid/gid.
 * Returning zero would falsely report superuser privileges to native code. */
uint32_t bs_bionic_getuid(void) {
#ifdef __SWITCH__
    errno = ENOSYS;
    return UINT32_MAX;
#else
    return (uint32_t)getuid();
#endif
}
uint32_t bs_bionic_geteuid(void) {
#ifdef __SWITCH__
    errno = ENOSYS;
    return UINT32_MAX;
#else
    return (uint32_t)geteuid();
#endif
}
uint32_t bs_bionic_getgid(void) {
#ifdef __SWITCH__
    errno = ENOSYS;
    return UINT32_MAX;
#else
    return (uint32_t)getgid();
#endif
}
uint32_t bs_bionic_getegid(void) {
#ifdef __SWITCH__
    errno = ENOSYS;
    return UINT32_MAX;
#else
    return (uint32_t)getegid();
#endif
}

int bs_bionic_gethostname(char *name, size_t capacity) {
    if (!name) { errno = EFAULT; return -1; }
    if (!capacity) { errno = EINVAL; return -1; }
    /* Use the actual OS networking name if available; preserve its error. */
    return gethostname(name, capacity);
}

int bs_bionic_uname(BsBionicUtsname *output) {
    if (!output) { errno = EFAULT; return -1; }
#ifdef __SWITCH__
    BsBionicUtsname result = {0};
    snprintf(result.sysname, sizeof result.sysname, "%s", "Horizon");
    snprintf(result.machine, sizeof result.machine, "%s", "aarch64");
    snprintf(result.version, sizeof result.version, "%s", "Horizon OS");
    uint32_t version = hosversionGet();
    if (version) {
        snprintf(result.release, sizeof result.release, "%u.%u.%u",
                 HOSVER_MAJOR(version), HOSVER_MINOR(version), HOSVER_MICRO(version));
    } else {
        snprintf(result.release, sizeof result.release, "%s", "unknown");
    }
    if (bs_bionic_gethostname(result.nodename, sizeof result.nodename) < 0)
        snprintf(result.nodename, sizeof result.nodename, "%s", "unknown");
    result.nodename[sizeof result.nodename - 1] = '\0';
    *output = result;
    return 0;
#else
    /* Host-side adapter tests only; no claim of an Android or Switch kernel. */
    errno = ENOSYS;
    return -1;
#endif
}
