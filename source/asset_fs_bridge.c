/* Android filesystem/AssetManager compatibility for the original Beat Saber
 * Quest APK/OBB. No game data is compiled into homebrew.
 *
 * Android Bionic's stat and dirent layouts differ from newlib. The functions
 * below translate them and resolve legitimate app paths under an isolated
 * private SD folder. Unknown Android system paths return real ENOENT errors.
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "config.h"
#include "asset_fs_bridge.h"
#include "jni_bridge.h"
#include "so_util.h"
#include "util.h"
#include "android_window_bridge.h"
#include "bionic_posix_bridge.h"

#define BS_ROOT GAME_HOME
#define BS_PERSISTENT GAME_HOME "/persistent"
#define BS_PATH_MAX 768
#define BS_EXTERNAL_FILES "/storage/emulated/0/Android/data/com.beatgames.beatsaber/files/"
#define BS_PRIVATE_FILES "/data/data/com.beatgames.beatsaber/files/"
#define BS_USER_FILES "/data/user/0/com.beatgames.beatsaber/files/"
#define BS_UNIX_ROOT "/switch/beat-saber-quest"

static const char *bs_synthetic_cpu_text(const char *path) {
    static const char cpuinfo[] =
        "processor\t: 0\nBogoMIPS\t: 38.40\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
        "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\nCPU part\t: 0xd07\nCPU revision\t: 1\n\n"
        "processor\t: 1\nBogoMIPS\t: 38.40\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
        "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\nCPU part\t: 0xd07\nCPU revision\t: 1\n\n"
        "processor\t: 2\nBogoMIPS\t: 38.40\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
        "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\nCPU part\t: 0xd07\nCPU revision\t: 1\n\n"
        "processor\t: 3\nBogoMIPS\t: 38.40\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
        "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\nCPU part\t: 0xd07\nCPU revision\t: 1\n\n"
        "Hardware\t: Nintendo Switch\n";
    static const char sys_cpu[] = "/sys/devices/system/cpu/";
    if (!strcmp(path, "/proc/cpuinfo")) return cpuinfo;
    if (strncmp(path, sys_cpu, sizeof sys_cpu - 1)) return NULL;
    const char *rest = path + sizeof sys_cpu - 1;
    if (!strcmp(rest, "possible") || !strcmp(rest, "present") ||
        !strcmp(rest, "online") || !strcmp(rest, "kernel_max"))
        return !strcmp(rest, "kernel_max") ? "3\n" : "0-3\n";
    if (strncmp(rest, "cpu", 3) || rest[3] < '0' || rest[3] > '3' || rest[4] != '/')
        return NULL;
    rest += 5;
    if (!strcmp(rest, "cpu_capacity")) return "1024\n";
    if (!strcmp(rest, "online")) return "1\n";
    if (!strcmp(rest, "cpufreq/cpuinfo_max_freq") ||
        !strcmp(rest, "cpufreq/scaling_max_freq") ||
        !strcmp(rest, "cpufreq/scaling_cur_freq") ||
        !strcmp(rest, "cpufreq/cpuinfo_cur_freq")) return "1785000\n";
    if (!strcmp(rest, "cpufreq/cpuinfo_min_freq") ||
        !strcmp(rest, "cpufreq/scaling_min_freq")) return "1020000\n";
    if (!strcmp(rest, "topology/physical_package_id")) return "0\n";
    if (!strcmp(rest, "topology/core_siblings_list") ||
        !strcmp(rest, "topology/package_cpus_list")) return "0-3\n";
    if (!strcmp(rest, "regs/identification/midr_el1")) return "0x00000000411fd071\n";
    return NULL;
}

static const char *bs_synthetic_file(const char *path,
                                      char *target, size_t capacity) {
    if (!path) return NULL;
    const bool maps = !strcmp(path, "/proc/self/maps") ||
                      !strcmp(path, "/proc/self/smaps");
    const bool meminfo = !strcmp(path, "/proc/meminfo");
    const bool random = !strcmp(path, "/dev/urandom") ||
                        !strcmp(path, "/dev/random");
    /* Static Linux CPU topology for the Switch's 4 Cortex-A57 cores (3 are
     * available to applications, but Unity sizes job workers from these). */
    const char *text = bs_synthetic_cpu_text(path);
    if (!maps && !meminfo && !random && !text) return NULL;
    {
        static int reports;
        if (text && reports < 48) {
            ++reports;
            debugPrintf("[fs] synthetic %s\n", path);
        }
    }
    u64 thread = 0;
    svcGetThreadId(&thread, CUR_THREAD_HANDLE);
    if (text) {
        /* Static text gets one backing file per path. Unity keeps
         * /proc/cpuinfo open and seeks back to re-read it; a per-thread
         * file shared by every CPU path would be overwritten in between
         * (by cpu_capacity etc.), leaving Unity with zero usable cores and
         * a job system that never wakes its workers. */
        uint32_t hash = 2166136261u;
        for (const char *c = path; *c; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
        int n = snprintf(target, capacity, BS_ROOT "/.android_cpu_%08x", hash);
        if (n < 0 || (size_t)n >= capacity) return NULL;
        struct stat st;
        if (!stat(target, &st) && (size_t)st.st_size == strlen(text)) return target;
        char temp[BS_PATH_MAX];
        n = snprintf(temp, sizeof temp, "%s.%llx", target, (unsigned long long)thread);
        if (n < 0 || (size_t)n >= sizeof temp) return NULL;
        FILE *output = fopen(temp, "wb");
        if (!output) return NULL;
        (void)fputs(text, output);
        fclose(output);
        if (rename(temp, target)) {
            /* Another thread installed it first, or it is open: either way
             * an identical copy exists. */
            remove(temp);
            if (stat(target, &st) || (size_t)st.st_size != strlen(text)) return NULL;
        }
        return target;
    }
    int n = snprintf(target, capacity, BS_ROOT "/.android_%s_%llx",
                     maps ? "maps" : meminfo ? "meminfo" : random ? "random" : "cpu",
                     (unsigned long long)thread);
    if (n < 0 || (size_t)n >= capacity) return NULL;
    FILE *output = fopen(target, "wb");
    if (!output) return NULL;
    if (maps) {
        char *mapping = malloc(32768);
        if (!mapping) { fclose(output); return NULL; }
        int length = so_dump_maps(mapping, 32768);
        if (length > 0)
            (void)fwrite(mapping, 1, (size_t)length, output);
        free(mapping);
    } else if (meminfo) {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        fprintf(output, "MemTotal: %llu kB\nMemAvailable: %llu kB\n",
                (unsigned long long)(total >> 10),
                (unsigned long long)((total > used ? total - used : 0) >> 10));
    } else {
        unsigned char entropy[4096];
        for (int i = 0; i < 16; ++i) {
            randomGet(entropy, sizeof entropy);
            (void)fwrite(entropy, 1, sizeof entropy, output);
        }
    }
    fclose(output);
    return target;
}

static const char *bs_android_path(const char *input, char *target,
                                   size_t target_size) {
    if (!input || !*input) { errno = EINVAL; return NULL; }
    const char *suffix = input;
    const char *prefix = BS_ROOT "/";
    if (strstr(input, "/../") || strstr(input, "\\..\\") ||
        !strcmp(input, "..") || !strncmp(input, "../", 3) ||
        (strlen(input) >= 3 && !strcmp(input + strlen(input) - 3, "/.."))) {
        errno = EACCES;
        return NULL;
    }
    {
        /* Unity scans "<apk|obb>/assets/bin/Data/UnitySubsystems" etc. with
         * the directory API; the archive members are extracted under the
         * game root's assets/ directory. */
        const char *member = strstr(input, ".obb/assets/");
        if (member) member += 5;
        else if ((member = strstr(input, ".apk/assets/"))) member += 5;
        if (member) {
            int n = snprintf(target, target_size, "%s/%s", BS_ROOT, member);
            return n > 0 && (size_t)n < target_size ? target : NULL;
        }
    }
    if (!strncmp(input, "sdmc:", 5)) return input;
    {
        /* Ancestors of the app's Android directories exist on a device;
         * recursive mkdir/stat walks them. Present them as the game root. */
        static const char *const ancestors[] = {
            "/", "/storage", "/storage/emulated", "/storage/emulated/0",
            "/storage/emulated/0/Android", "/storage/emulated/0/Android/data",
            "/storage/emulated/0/Android/data/com.beatgames.beatsaber",
            "/storage/emulated/0/Android/obb",
            "/storage/emulated/0/Android/obb/com.beatgames.beatsaber",
            "/data", "/data/data", "/data/data/com.beatgames.beatsaber",
            "/data/user", "/data/user/0", "/data/user/0/com.beatgames.beatsaber",
            "/data/app", "/data/app/com.beatgames.beatsaber",
            "/data/app/com.beatgames.beatsaber/lib",
            "/data/app/com.beatgames.beatsaber/lib/arm64",
        };
        size_t len = strlen(input);
        while (len > 1 && input[len - 1] == '/') --len;
        for (size_t i = 0; i < sizeof ancestors / sizeof ancestors[0]; ++i) {
            if (strlen(ancestors[i]) == len && !strncmp(input, ancestors[i], len)) {
                int n = snprintf(target, target_size, "%s", BS_ROOT);
                return n > 0 && (size_t)n < target_size ? target : NULL;
            }
        }
        /* Installed native libraries live directly in the game root. */
        static const char lib_dir[] = "/data/app/com.beatgames.beatsaber/lib/arm64/";
        if (!strncmp(input, lib_dir, sizeof lib_dir - 1)) {
            int n = snprintf(target, target_size, "%s/%s", BS_ROOT,
                             input + sizeof lib_dir - 1);
            return n > 0 && (size_t)n < target_size ? target : NULL;
        }
        /* Unity's il2cpp resource cache under an unresolved data dir. */
        if (!strncmp(input, "/il2cpp", 7) && (!input[7] || input[7] == '/')) {
            int n = snprintf(target, target_size, "%s%s", BS_PERSISTENT, input);
            return n > 0 && (size_t)n < target_size ? target : NULL;
        }
    }
    if (!strncmp(input, BS_EXTERNAL_FILES, sizeof(BS_EXTERNAL_FILES) - 1)) {
        suffix = input + sizeof(BS_EXTERNAL_FILES) - 1;
        prefix = BS_PERSISTENT "/";
    } else if (!strncmp(input, BS_PRIVATE_FILES, sizeof(BS_PRIVATE_FILES) - 1)) {
        suffix = input + sizeof(BS_PRIVATE_FILES) - 1;
        prefix = BS_PERSISTENT "/";
    } else if (!strncmp(input, BS_USER_FILES, sizeof(BS_USER_FILES) - 1)) {
        suffix = input + sizeof(BS_USER_FILES) - 1;
        prefix = BS_PERSISTENT "/";
    } else if (!strcmp(input, "/storage/emulated/0/Android/data/com.beatgames.beatsaber/files")) {
        suffix = "";
        prefix = BS_PERSISTENT;
    } else if (!strncmp(input, "/storage/emulated/0/Android/data/com.beatgames.beatsaber/", 57) ||
               !strncmp(input, "/data/data/com.beatgames.beatsaber/", 35) ||
               !strncmp(input, "/data/user/0/com.beatgames.beatsaber/", 37)) {
        /* Other package-private dirs (cache, shared_prefs, Unity's GUID
         * dir) sit beside the files dir, in the game root. */
        suffix = strstr(input, "com.beatgames.beatsaber/") + 24;
    } else if (!strncmp(input, BS_UNIX_ROOT "/", sizeof(BS_UNIX_ROOT))) {
        suffix = input + sizeof(BS_UNIX_ROOT);
    } else if (!strcmp(input, BS_UNIX_ROOT)) {
        suffix = "";
        prefix = BS_ROOT;
    } else if (!strncmp(input, "/assets/", 8)) {
        suffix = input + 1;
    } else if (!strncmp(input, "assets/", 7)) {
        suffix = input;
    } else if (input[0] == '/') {
        /* Unknown absolute Android path. Do not fabricate a root SD path. */
        static int unmapped_reports;
        if (unmapped_reports < 64) {
            ++unmapped_reports;
            debugPrintf("[fs] unmapped Android path %s -> ENOENT\n", input);
        }
        errno = ENOENT;
        return NULL;
    }
    int n = snprintf(target, target_size, "%s%s", prefix, suffix);
    if (n < 0 || (size_t)n >= target_size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return target;
}

static int bs_translate_flags(int flags) {
    /* Linux / Bionic open(2) flags are not native newlib flags. */
    int result = flags & 3;
    if (flags & 0100) result |= O_CREAT;
    if (flags & 0200) result |= O_EXCL;
    if (flags & 01000) result |= O_TRUNC;
    if (flags & 02000) result |= O_APPEND;
    if (flags & 04000) result |= O_NONBLOCK;
    return result;
}

/* newlib on Switch reports st_ino == 0 and st_dev == 0 for every file. il2cpp's
 * POSIX file layer keys share-mode checks on (dev, ino), so any two open files
 * looked identical ("Sharing violation" while saving settings.ini). Give each
 * file a stable synthetic inode: a hash of its native path (case-folded, since
 * FAT/exFAT are case-insensitive), tracked per descriptor for fstat. */
#define BS_INODE_FD_CAP 1024
#define BS_SYNTHETIC_DEV 0x5357u
static uint64_t g_fd_inode[BS_INODE_FD_CAP];
static uint64_t bs_path_inode(const char *p) {
    uint64_t h = 1469598103934665603ull;
    char prev = 0;
    for (; *p; ++p) {
        char ch = *p;
        if (ch == '/' && prev == '/') continue;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        h = (h ^ (uint8_t)ch) * 1099511628211ull;
        prev = ch;
    }
    return h | 1;
}

static int bs_open_path(const char *path, int flags, int mode) {
    char mapped[BS_PATH_MAX];
    const char *p = NULL;
    if ((flags & 3) == 0 && !(flags & 0100))
        p = bs_synthetic_file(path, mapped, sizeof mapped);
    if (!p)
        p = bs_android_path(path, mapped, sizeof mapped);
    if (!p) return -1;
    int fd = open(p, bs_translate_flags(flags), mode);
    if (fd >= 0) bs_bionic_note_open_fd(fd, p);
    if (fd >= 0 && fd < BS_INODE_FD_CAP) g_fd_inode[fd] = bs_path_inode(p);
    if (fd < 0 && errno != ENOENT)
        debugPrintf("[fs] open %s (%s) -> %d errno=%d\n", path, p, fd, errno);
    return fd;
}
int bs_android_open(const char *path, int flags, ...) {
    int mode = 0666;
    if (flags & 0100) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    return bs_open_path(path, flags, mode);
}
int bs_android_openat(int dirfd, const char *path, int flags, ...) {
    if (dirfd != -100 && path && path[0] != '/') {
        errno = ENOTSUP;
        return -1;
    }
    int mode = 0666;
    if (flags & 0100) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    return bs_open_path(path, flags, mode);
}

struct BsBionicTimespec { int64_t sec, nsec; };
struct BsBionicStat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad1;
    int64_t size;
    int32_t blksize, pad2;
    int64_t blocks;
    struct BsBionicTimespec atim, mtim, ctim;
    uint32_t unused4, unused5;
};
static void bs_convert_stat(const struct stat *src, void *output) {
    struct BsBionicStat *out = output;
    memset(out, 0, sizeof(*out));
    out->dev = src->st_dev;
    out->ino = src->st_ino;
    out->mode = src->st_mode;
    out->nlink = src->st_nlink;
    out->uid = src->st_uid;
    out->gid = src->st_gid;
    out->rdev = src->st_rdev;
    out->size = src->st_size;
    out->blksize = src->st_blksize;
    out->blocks = src->st_blocks;
    out->atim.sec = src->st_atime;
    out->mtim.sec = src->st_mtime;
    out->ctim.sec = src->st_ctime;
}
int bs_android_stat(const char *path, void *output) {
    if (!output) { errno = EINVAL; return -1; }
    char translated[BS_PATH_MAX];
    const char *p = bs_android_path(path, translated, sizeof translated);
    if (!p) return -1;
    struct stat st;
    int rc = stat(p, &st);
    if (!rc) {
        bs_convert_stat(&st, output);
        struct BsBionicStat *o = output;
        if (!o->ino) { o->ino = bs_path_inode(p); o->dev = BS_SYNTHETIC_DEV; }
    }
    return rc;
}
int bs_android_fstat(int fd, void *output) {
    if (!output) { errno = EINVAL; return -1; }
    struct stat st;
    int rc = fstat(fd, &st);
    if (!rc) {
        bs_convert_stat(&st, output);
        struct BsBionicStat *o = output;
        if (!o->ino) {
            o->ino = (fd >= 0 && fd < BS_INODE_FD_CAP && g_fd_inode[fd])
                         ? g_fd_inode[fd] : 0x100000000ull + (uint64_t)fd;
            o->dev = BS_SYNTHETIC_DEV;
        }
    }
    return rc;
}
int bs_android_lstat(const char *path, void *output) {
    /* FAT32 mounts do not support symbolic links. */
    return bs_android_stat(path, output);
}
int bs_android_mkdir(const char *path, unsigned mode) {
    char translated[BS_PATH_MAX];
    const char *p = bs_android_path(path, translated, sizeof translated);
    if (!p) return -1;
    return mkdir(p, mode);
}
int bs_android_access(const char *path, int mode) {
    char translated[BS_PATH_MAX];
    const char *p = bs_android_path(path, translated, sizeof translated);
    if (!p) return -1;
    return access(p, mode);
}
int bs_android_chmod(const char *path, unsigned mode) {
    char translated[BS_PATH_MAX];
    const char *p = bs_android_path(path, translated, sizeof translated);
    return p ? chmod(p, (mode_t)mode) : -1;
}
int bs_android_fchmod(int fd, unsigned mode) {
    /* libnx's virtual Android pipe descriptors have no filesystem mode. */
    if (fd >= 0x4000) { errno = EBADF; return -1; }
    return fchmod(fd, (mode_t)mode);
}
/* Bionic's relative *at operations are valid against AT_FDCWD. A genuine
 * descriptor-relative path requires the host descriptor's directory object;
 * attempting to interpret an integer fd as a pathname could mutate an
 * unrelated private asset. Android's AT_REMOVEDIR and AT_SYMLINK_NOFOLLOW
 * constants are deliberately decoded independently of newlib headers. */
#define BS_ANDROID_AT_FDCWD (-100)
#define BS_ANDROID_AT_REMOVEDIR 0x200
#define BS_ANDROID_AT_SYMLINK_NOFOLLOW 0x100
static const char *bs_android_at_path(int dirfd, const char *path,
                                     char mapped[BS_PATH_MAX]) {
    if (!path) { errno = EFAULT; return NULL; }
    if (dirfd != BS_ANDROID_AT_FDCWD && path[0] != '/' &&
        strncmp(path, "sdmc:", 5)) {
        errno = ENOTSUP;
        return NULL;
    }
    return bs_android_path(path, mapped, BS_PATH_MAX);
}
int bs_android_fchmodat(int dirfd, const char *path, unsigned mode, int flags) {
    if (flags & ~BS_ANDROID_AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    /* fsdev lacks lchmod and cannot promise symlink non-follow semantics. */
    if (flags) { errno = ENOTSUP; return -1; }
    char mapped[BS_PATH_MAX];
    const char *p = bs_android_at_path(dirfd, path, mapped);
    return p ? chmod(p, (mode_t)mode) : -1;
}
int bs_android_unlinkat(int dirfd, const char *path, int flags) {
    if (flags & ~BS_ANDROID_AT_REMOVEDIR) {
        errno = EINVAL;
        return -1;
    }
    char mapped[BS_PATH_MAX];
    const char *p = bs_android_at_path(dirfd, path, mapped);
    if (!p) return -1;
    return flags == BS_ANDROID_AT_REMOVEDIR ? rmdir(p) : unlink(p);
}
int bs_android_unlink(const char *path) {
    char mapped[BS_PATH_MAX];
    const char *p = bs_android_path(path, mapped, sizeof mapped);
    return p ? unlink(p) : -1;
}
int bs_android_rmdir(const char *path) {
    char mapped[BS_PATH_MAX];
    const char *p = bs_android_path(path, mapped, sizeof mapped);
    return p ? rmdir(p) : -1;
}
int bs_android_remove(const char *path) {
    char mapped[BS_PATH_MAX];
    const char *p = bs_android_path(path, mapped, sizeof mapped);
    return p ? remove(p) : -1;
}
/* POSIX rename atomically replaces an existing destination file; fsdev fails
 * with EEXIST instead. Emulate: replace a regular-file destination (or empty
 * directory over directory) by removing it first. Not atomic, but the
 * source is never lost: it is only moved after the destination is gone. */
int bs_android_rename(const char *oldpath, const char *newpath) {
    char old_mapped[BS_PATH_MAX], new_mapped[BS_PATH_MAX];
    const char *o = bs_android_path(oldpath, old_mapped, sizeof old_mapped);
    const char *n = o ? bs_android_path(newpath, new_mapped, sizeof new_mapped) : NULL;
    if (!o || !n) return -1;
    if (!rename(o, n)) return 0;
    int error = errno;
    struct stat src, dst;
    if (stat(o, &src) != 0 || stat(n, &dst) != 0) { errno = error; return -1; }
    if (!strcasecmp(o, n)) return 0;
    if (S_ISDIR(src.st_mode) != S_ISDIR(dst.st_mode)) {
        errno = S_ISDIR(dst.st_mode) ? EISDIR : ENOTDIR;
        return -1;
    }
    if ((S_ISDIR(dst.st_mode) ? rmdir(n) : unlink(n)) != 0) return -1;
    return rename(o, n);
}
int bs_android_renameat(int olddirfd, const char *oldpath, int newdirfd,
                        const char *newpath) {
    if ((olddirfd != BS_ANDROID_AT_FDCWD && oldpath && oldpath[0] != '/') ||
        (newdirfd != BS_ANDROID_AT_FDCWD && newpath && newpath[0] != '/')) {
        errno = ENOTSUP;
        return -1;
    }
    return bs_android_rename(oldpath, newpath);
}
int bs_android_link(const char *oldpath, const char *newpath) {
    char old_mapped[BS_PATH_MAX], new_mapped[BS_PATH_MAX];
    if (!bs_android_path(oldpath, old_mapped, sizeof old_mapped) ||
        !bs_android_path(newpath, new_mapped, sizeof new_mapped)) return -1;
    /* The Switch SD's filesystem and libnx fsdev have no hard-link API. */
    errno = ENOTSUP;
    return -1;
}
int bs_android_symlink(const char *target, const char *linkpath) {
    char mapped[BS_PATH_MAX];
    if (!target) { errno = EINVAL; return -1; }
    if (!bs_android_path(linkpath, mapped, sizeof mapped)) return -1;
    errno = ENOTSUP;
    return -1;
}
ssize_t bs_android_readlink(const char *path, char *buffer, size_t capacity) {
    char mapped[BS_PATH_MAX];
    const char *p = bs_android_path(path, mapped, sizeof mapped);
    if (!p) return -1;
    if (!buffer || !capacity) { errno = EINVAL; return -1; }
    /* fsdev does not expose POSIX symlinks: distinguish a missing path from
     * a real regular file, which Linux readlink rejects with EINVAL. */
    struct stat file;
    if (stat(p, &file) != 0) return -1;
    errno = EINVAL;
    return -1;
}
ssize_t bs_android_sendfile(int out_fd, int in_fd, int64_t *offset,
                            size_t count) {
    if (out_fd < 0 || in_fd < 0) { errno = EBADF; return -1; }
    struct stat source;
    if (fstat(in_fd, &source) != 0) return -1;
    if (!S_ISREG(source.st_mode)) { errno = EINVAL; return -1; }
    if (count > SSIZE_MAX) count = SSIZE_MAX;
    off_t old_position = 0;
    if (offset) {
        if (*offset < 0) { errno = EINVAL; return -1; }
        if ((uint64_t)*offset > INT64_MAX - count) {
            errno = EOVERFLOW;
            return -1;
        }
        old_position = lseek(in_fd, 0, SEEK_CUR);
        if (old_position == (off_t)-1) return -1;
        if ((int64_t)(off_t)*offset != *offset) {
            errno = EOVERFLOW;
            return -1;
        }
        if (lseek(in_fd, (off_t)*offset, SEEK_SET) == (off_t)-1)
            return -1;
    }
    unsigned char chunk[8192];
    size_t transferred = 0;
    int saved_error = 0;
    while (transferred < count) {
        size_t remaining = count - transferred;
        size_t wanted = remaining < sizeof chunk ? remaining : sizeof chunk;
        ssize_t n = bs_pipe_read(in_fd, chunk, wanted);
        if (n == 0) break;
        if (n < 0) { saved_error = errno; break; }
        ssize_t written = 0;
        while (written < n) {
            ssize_t next = bs_pipe_write(out_fd, chunk + written,
                                          (size_t)(n - written));
            if (next <= 0) {
                saved_error = next == 0 ? EIO : errno;
                break;
            }
            written += next;
        }
        transferred += (size_t)written;
        if (written != n) {
            /* The native read consumed bytes that the output could not
             * accept. Linux sendfile advances only by bytes transferred. */
            if (lseek(in_fd, written - n, SEEK_CUR) == (off_t)-1 &&
                !saved_error) saved_error = errno;
            break;
        }
    }
    if (offset) {
        *offset += (int64_t)transferred;
        if (lseek(in_fd, old_position, SEEK_SET) == (off_t)-1 &&
            !saved_error) saved_error = errno;
    }
    if (saved_error && transferred == 0) { errno = saved_error; return -1; }
    return (ssize_t)transferred;
}
int bs_android_dup2(int oldfd, int newfd) {
    /* The ALooper bridge owns virtual descriptors above 0x4000. newlib's
     * descriptor table cannot duplicate those synthetic pipe endpoints. */
    if (oldfd >= 0x4000 || newfd >= 0x4000) {
        errno = EBADF;
        return -1;
    }
    int fd = dup2(oldfd, newfd);
    if (fd >= 0 && oldfd != newfd)
        bs_bionic_note_dup2_fd(oldfd, newfd);
    return fd;
}
FILE *bs_android_fopen(const char *path, const char *mode) {
    char translated[BS_PATH_MAX];
    const char *p = NULL;
    if (mode && mode[0] == 'r')
        p = bs_synthetic_file(path, translated, sizeof translated);
    if (!p)
        p = bs_android_path(path, translated, sizeof translated);
    FILE *f = p ? fopen(p, mode) : NULL;
    if (!f && p && errno != ENOENT)
        debugPrintf("[fs] fopen %s (%s) failed errno=%d\n", path, p, errno);
    return f;
}
void *bs_android_opendir(const char *path) {
    char translated[BS_PATH_MAX];
    const char *p = bs_android_path(path, translated, sizeof translated);
    return p ? opendir(p) : NULL;
}
struct BsBionicDirent {
    uint64_t ino;
    int64_t offset;
    uint16_t record_length;
    uint8_t type;
    char name[256];
};
void *bs_android_readdir(void *dir) {
    if (!dir) { errno = EINVAL; return NULL; }
    struct dirent *native = readdir((DIR *)dir);
    if (!native) return NULL;
    /* One shared record let simultaneous Unity filesystem enumerations
     * overwrite each other's results before the caller read d_name. */
    static _Thread_local struct BsBionicDirent out;
    memset(&out, 0, sizeof out);
    out.ino = native->d_ino;
    out.record_length = sizeof out;
    out.type = native->d_type; /* newlib DT_* values match Linux/bionic */
    snprintf(out.name, sizeof(out.name), "%s", native->d_name);
    return &out;
}
char *bs_android_realpath(const char *path, char *resolved) {
    if (!resolved) { errno = EINVAL; return NULL; }
    char translated[BS_PATH_MAX];
    const char *p = bs_android_path(path, translated, sizeof translated);
    if (!p) return NULL;
    return realpath(p, resolved);
}
char *bs_android_getcwd(char *buffer, size_t capacity) {
    char *found = getcwd(buffer, capacity);
    if (found && !strncmp(found, "sdmc:", 5))
        memmove(found, found + 5, strlen(found + 5) + 1);
    return found;
}
char *bs_android_getenv(const char *key) {
    if (!key) return NULL;
    if (!strcmp(key, "HOME") || !strcmp(key, "TMPDIR"))
        return (char *)BS_UNIX_ROOT "/persistent";
    /* IL2CPP's Boehm GC stops the world with pthread_kill(SIG_SUSPEND) and
     * a handler that must run on each target thread; Horizon has no
     * asynchronous signal delivery. Until cooperative suspension exists,
     * start the GC disabled (heap grows instead of collecting). Boehm reads
     * this in GC_init; GC_enable/GC_disable pairs stay balanced. */
    if (!strcmp(key, "GC_DONT_GC")) return (char *)"1";
    return getenv(key);
}
int bs_android_chdir(const char *path) {
    char translated[BS_PATH_MAX];
    const char *p = bs_android_path(path, translated, sizeof translated);
    return p ? chdir(p) : -1;
}

unsigned char bs_bionic_streams[3][256];
FILE *bs_bionic_stdin = (FILE *)&bs_bionic_streams[0][0];
FILE *bs_bionic_stdout = (FILE *)&bs_bionic_streams[1][0];
FILE *bs_bionic_stderr = (FILE *)&bs_bionic_streams[2][0];
static int bs_fake_stdio_index(FILE *f) {
    uintptr_t ptr = (uintptr_t)f;
    uintptr_t begin = (uintptr_t)&bs_bionic_streams[0][0];
    if (ptr < begin || ptr >= begin + sizeof(bs_bionic_streams))
        return -1;
    return (int)((ptr - begin) / 256);
}
static FILE *bs_real_stdio(FILE *f) {
    int idx = bs_fake_stdio_index(f);
    if (idx == 0) return stdin;
    if (idx == 1) return stdout;
    if (idx == 2) return stderr;
    return f;
}
size_t bs_android_fwrite(const void *p, size_t sz, size_t count, FILE *f) {
    return fwrite(p, sz, count, bs_real_stdio(f));
}
size_t bs_android_fread(void *p, size_t sz, size_t count, FILE *f) {
    return fread(p, sz, count, bs_real_stdio(f));
}
int bs_android_fclose(FILE *f) {
    if (bs_fake_stdio_index(f) >= 0) return 0;
    return fclose(f);
}
int bs_android_fflush(FILE *f) {
    return fflush(f ? bs_real_stdio(f) : NULL);
}
int bs_android_vfprintf(FILE *f, const char *format, va_list args) {
    return vfprintf(bs_real_stdio(f), format, args);
}
int bs_android_fprintf(FILE *f, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = bs_android_vfprintf(f, format, args);
    va_end(args);
    return result;
}
int bs_android_fputs(const char *message, FILE *f) {
    return fputs(message, bs_real_stdio(f));
}
int bs_android_fputc(int ch, FILE *f) {
    return fputc(ch, bs_real_stdio(f));
}
int bs_android_ferror(FILE *f) { return ferror(bs_real_stdio(f)); }
int bs_android_feof(FILE *f) { return feof(bs_real_stdio(f)); }
int bs_android_fileno(FILE *f) { return fileno(bs_real_stdio(f)); }
int bs_android_fseek(FILE *f, long off, int whence) {
    return fseek(bs_real_stdio(f), off, whence);
}
long bs_android_ftell(FILE *f) { return ftell(bs_real_stdio(f)); }
int bs_android_vfscanf(FILE *f, const char *format, va_list args) {
    if (!f || !format) { errno = EINVAL; return EOF; }
    return vfscanf(bs_real_stdio(f), format, args);
}
int bs_android_fscanf(FILE *f, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = bs_android_vfscanf(f, format, args);
    va_end(args);
    return result;
}
char *bs_android_fgets(char *buffer, int capacity, FILE *stream) {
    if (!buffer || capacity <= 0 || !stream) {
        errno = EINVAL;
        return NULL;
    }
    return fgets(buffer, capacity, bs_real_stdio(stream));
}
FILE *bs_android_fdopen(int fd, const char *mode) {
    if (fd < 0 || !mode) {
        errno = EINVAL;
        return NULL;
    }
    /* Real newlib file descriptors are compatible with fdopen. Custom
     * Horizon virtual pipes that are not newlib descriptors fail EBADF. */
    return fdopen(fd, mode);
}
void bs_android_setbuf(FILE *stream, char *buffer) {
    if (stream) setbuf(bs_real_stdio(stream), buffer);
}
void bs_android_clearerr(FILE *stream) {
    if (stream) clearerr(bs_real_stdio(stream));
}
void bs_android_perror(const char *label) {
    const int saved_errno = errno;
    debugPrintf("[android:perror] %s: %s (errno=%d)\n",
                label ? label : "", strerror(saved_errno), saved_errno);
    errno = saved_errno;
}
int bs_android_fseeko(FILE *f, off_t offset, int origin) {
    if (!f) { errno = EINVAL; return -1; }
    return fseeko(bs_real_stdio(f), offset, origin);
}
off_t bs_android_ftello(FILE *f) {
    if (!f) { errno = EINVAL; return -1; }
    return ftello(bs_real_stdio(f));
}
int bs_android_getc(FILE *f) {
    if (!f) { errno = EINVAL; return EOF; }
    return fgetc(bs_real_stdio(f));
}
int bs_android_ungetc(int ch, FILE *f) {
    if (!f) { errno = EINVAL; return EOF; }
    return ungetc(ch, bs_real_stdio(f));
}
wint_t bs_android_getwc(FILE *f) {
    if (!f) { errno = EINVAL; return WEOF; }
    return fgetwc(bs_real_stdio(f));
}
wint_t bs_android_ungetwc(wint_t ch, FILE *f) {
    if (!f) { errno = EINVAL; return WEOF; }
    return ungetwc(ch, bs_real_stdio(f));
}
wint_t bs_android_fputwc(wchar_t ch, FILE *f) {
    if (!f) { errno = EINVAL; return WEOF; }
    return fputwc(ch, bs_real_stdio(f));
}

/* NDK AAssetManager operations read original staged APK+OBB files. */
static int g_asset_manager;
typedef struct BsAsset {
    FILE *file;
    int64_t length;
    void *mapped_bytes;
} BsAsset;
typedef struct BsAssetDir {
    DIR *dir;
    char path[BS_PATH_MAX];
    char name[256];
} BsAssetDir;
void *bs_android_assetmanager_from_java(void *env, void *jobject) {
    if (!env || env != bs_jni_env() || !jobject ||
        jobject != bs_jni_asset_manager()) {
        debugPrintf("[assets] AAssetManager_fromJava(%p) rejected (expected %p)\n",
                    jobject, bs_jni_asset_manager());
        return NULL;
    }
    return &g_asset_manager;
}
void *bs_android_assetmanager_open(void *manager, const char *name, int mode) {
    (void)mode;
    if (manager != &g_asset_manager || !name) {
        errno = EINVAL;
        return NULL;
    }
    char buffer[BS_PATH_MAX];
    int written = snprintf(buffer, sizeof buffer, "assets/%s", name);
    if (written < 0 || written >= (int)sizeof buffer) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    FILE *file = bs_android_fopen(buffer, "rb");
    if (!file) {
        debugPrintf("[assets] AAssetManager_open(%s) missing\n", name);
        return NULL;
    }
    BsAsset *asset = calloc(1, sizeof *asset);
    if (!asset) { fclose(file); errno = ENOMEM; return NULL; }
    asset->file = file;
    if (fseeko(file, 0, SEEK_END) != 0) {
        int saved_errno = errno;
        fclose(file); free(asset);
        errno = saved_errno;
        return NULL;
    }
    off_t length = ftello(file);
    if (length < 0 || fseeko(file, 0, SEEK_SET) != 0) {
        int saved_errno = errno;
        fclose(file); free(asset);
        errno = saved_errno ? saved_errno : EIO;
        return NULL;
    }
    asset->length = (int64_t)length;
    return asset;
}
void *bs_android_assetmanager_open_dir(void *manager, const char *name) {
    if (manager != &g_asset_manager || !name) {
        errno = EINVAL;
        return NULL;
    }
    char path[BS_PATH_MAX];
    if (snprintf(path, sizeof path, "assets/%s", name) >= (int)sizeof path) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char native_path[BS_PATH_MAX];
    const char *resolved = bs_android_path(path, native_path, sizeof native_path);
    if (!resolved) return NULL;
    DIR *directory = opendir(resolved);
    if (!directory) return NULL;
    BsAssetDir *asset_dir = calloc(1, sizeof *asset_dir);
    if (!asset_dir) {
        closedir(directory);
        errno = ENOMEM;
        return NULL;
    }
    asset_dir->dir = directory;
    memcpy(asset_dir->path, native_path, strlen(native_path) + 1);
    return asset_dir;
}
const char *bs_android_asset_dir_get_next_file_name(void *ptr) {
    BsAssetDir *directory = ptr;
    if (!directory || !directory->dir) {
        errno = EINVAL;
        return NULL;
    }
    struct dirent *entry;
    while ((entry = readdir(directory->dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        size_t length = strlen(entry->d_name);
        if (length >= sizeof directory->name) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        /* NdkAssetManager's getNextFileName enumerates FILES, not directory
         * names. Reject subdirectories even when fsdev leaves d_type unset. */
        char file_path[BS_PATH_MAX];
        int written = snprintf(file_path, sizeof file_path, "%s/%s",
                               directory->path, entry->d_name);
        if (written < 0 || written >= (int)sizeof file_path) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        struct stat st;
        if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        memcpy(directory->name, entry->d_name, length + 1);
        return directory->name;
    }
    return NULL; /* NDK: NULL at end of directory enumeration. */
}
void bs_android_asset_dir_rewind(void *ptr) {
    BsAssetDir *directory = ptr;
    if (directory && directory->dir)
        rewinddir(directory->dir);
}
void bs_android_asset_dir_close(void *ptr) {
    BsAssetDir *directory = ptr;
    if (!directory) return;
    if (directory->dir) closedir(directory->dir);
    free(directory);
}
void bs_android_asset_close(void *ptr) {
    BsAsset *asset = ptr;
    if (asset) {
        free(asset->mapped_bytes);
        if (asset->file) fclose(asset->file);
        free(asset);
    }
}
int bs_android_asset_read(void *ptr, void *buffer, size_t count) {
    BsAsset *asset = ptr;
    if (!asset || !asset->file || (!buffer && count)) {
        errno = EINVAL;
        return -1;
    }
    if (!count) return 0;
    if (count > INT32_MAX) count = INT32_MAX;
    size_t actual = fread(buffer, 1, count, asset->file);
    /* AAsset_read returns -1 on an I/O error, while a clean EOF is zero. */
    if (ferror(asset->file)) { errno = EIO; return -1; }
    return (int)actual;
}
int64_t bs_android_asset_seek64(void *ptr, int64_t offset, int whence) {
    BsAsset *asset = ptr;
    if (!asset || !asset->file) { errno = EINVAL; return -1; }
    if ((int64_t)(off_t)offset != offset) { errno = EOVERFLOW; return -1; }
    if (fseeko(asset->file, (off_t)offset, whence) != 0) return -1;
    return (int64_t)ftello(asset->file);
}
int64_t bs_android_asset_length64(void *ptr) {
    return ptr ? ((BsAsset *)ptr)->length : -1;
}
int64_t bs_android_asset_length(void *ptr) {
    return bs_android_asset_length64(ptr);
}
const void *bs_android_asset_get_buffer(void *ptr) {
    BsAsset *asset = ptr;
    if (!asset || !asset->file || asset->length < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (asset->mapped_bytes) return asset->mapped_bytes;
    if ((uint64_t)asset->length > SIZE_MAX - 1) {
        errno = EOVERFLOW;
        return NULL;
    }
    /* Android returns an asset-owned pointer valid until AAsset_close.
     * Materialize the unmodified original file and preserve the stream's
     * current read offset. Never fabricate asset contents on failed reads.
     */
    off_t original_offset = ftello(asset->file);
    if (original_offset < 0) return NULL;
    void *contents = malloc((size_t)asset->length + 1);
    if (!contents) { errno = ENOMEM; return NULL; }
    if (fseeko(asset->file, 0, SEEK_SET) != 0) {
        free(contents);
        return NULL;
    }
    size_t actual = fread(contents, 1, (size_t)asset->length, asset->file);
    bool read_failed = actual != (size_t)asset->length || ferror(asset->file);
    if (fseeko(asset->file, original_offset, SEEK_SET) != 0) read_failed = true;
    if (read_failed) {
        free(contents);
        errno = EIO;
        return NULL;
    }
    ((unsigned char *)contents)[asset->length] = 0;
    asset->mapped_bytes = contents;
    return contents;
}

typedef struct BsAConfiguration {
    uint32_t magic, sdk;
    char language[4], country[4];
} BsAConfiguration;
void *bs_android_configuration_new(void) {
    BsAConfiguration *config = calloc(1, sizeof(*config));
    if (!config) return NULL;
    /* The emulated Android API level is a compatibility property; the
     * device's language and territory are not known until a real Horizon
     * settings observation succeeds. AConfiguration_new has no locale yet. */
    *config = (BsAConfiguration){.magic=0x42414346,.sdk=32};
    return config;
}
void bs_android_configuration_delete(void *config) { free(config); }
void bs_android_configuration_from_assetmanager(void *config, void *manager) {
    if (!config || manager != &g_asset_manager) return;
    BsAConfiguration *out = config;
    if (out->magic != 0x42414346) return;
    /* Switch Settings.LanguageCode is a NUL-terminated ASCII BCP-47 tag in
     * an eight-byte little-endian integer, e.g. en-US, fr-CA or zh-Hans.
     * Android's NDK AConfiguration getters take two-letter ISO language and
     * country codes. Never manufacture a locale when Settings is unavailable. */
    u64 language_code = 0;
    if (R_FAILED(setInitialize())) return;
    Result result = setGetSystemLanguage(&language_code);
    setExit();
    if (R_FAILED(result)) return;
    char tag[9] = {0};
    memcpy(tag, &language_code, sizeof(language_code));
    const unsigned char a = (unsigned char)tag[0];
    const unsigned char b = (unsigned char)tag[1];
    if (a < 'a' || a > 'z' || b < 'a' || b > 'z') return;
    char country[4] = {0};
    if ((tag[2] == '-' || tag[2] == '_') &&
        tag[3] >= 'A' && tag[3] <= 'Z' &&
        tag[4] >= 'A' && tag[4] <= 'Z' && tag[5] == '\0') {
        country[0] = tag[3];
        country[1] = tag[4];
    } else if (!strncmp(tag, "zh-Hans", 7)) {
        memcpy(country, "CN", 3);
    } else if (!strncmp(tag, "zh-Hant", 7)) {
        memcpy(country, "TW", 3);
    }
    out->language[0] = tag[0];
    out->language[1] = tag[1];
    out->language[2] = 0;
    memcpy(out->country, country, sizeof country);
}
void bs_android_configuration_get_language(void *config, char *out) {
    if (config && out && ((BsAConfiguration *)config)->magic == 0x42414346)
        memcpy(out, ((BsAConfiguration *)config)->language, 3);
}
void bs_android_configuration_get_country(void *config, char *out) {
    if (config && out && ((BsAConfiguration *)config)->magic == 0x42414346)
        memcpy(out, ((BsAConfiguration *)config)->country, 3);
}
int bs_android_configuration_get_sdk_version(void *config) {
    return config && ((BsAConfiguration *)config)->magic == 0x42414346 ?
           (int)((BsAConfiguration *)config)->sdk : 0;
}
