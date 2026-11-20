/* Host behavior tests for the AArch64 Bionic POSIX bridge.
 * Build on Linux (including WSL) with -Werror and source/bionic_posix_bridge.c.
 * Stubs supply the Android path resolver and open registration;
 * all POSIX directory operations and descriptor ownership are real.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "bionic_posix_bridge.h"

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static char sandbox[PATH_MAX];
static char native_file[PATH_MAX];

char *bs_android_realpath(const char *path, char *resolved) {
    if (!path || strncmp(path, "/guest/", 7) || strstr(path, "..")) {
        errno = ENOENT;
        return NULL;
    }
    char joined[PATH_MAX];
    if (snprintf(joined, sizeof joined, "%s/%s", sandbox, path + 7) >=
        (int)sizeof joined) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return realpath(joined, resolved);
}

int bs_android_open(const char *path, int flags, ...) {
    char native[PATH_MAX];
    if (!bs_android_realpath(path, native)) return -1;
    int fd = open(native, flags & 3);
    if (fd >= 0) bs_bionic_note_open_fd(fd, native);
    return fd;
}

static void check_file_length(int fd, off_t length) {
    struct stat st;
    assert(fstat(fd, &st) == 0);
    assert(st.st_size == length);
}

static void test_fd_operations(void) {
    int fd = open(native_file, O_RDWR);
    assert(fd >= 0);
    assert(write(fd, "abcdef", 6) == 6);
    assert(lseek(fd, 2, SEEK_SET) == 2);

    char positioned[4] = {0};
    assert(bs_bionic_pread(fd, positioned, 3, 1) == 3);
    assert(strcmp(positioned, "bcd") == 0);
    assert(lseek(fd, 0, SEEK_CUR) == 2);
    assert(bs_bionic_pwrite(fd, "XY", 2, 3) == 2);
    assert(lseek(fd, 0, SEEK_CUR) == 2);
    memset(positioned, 0, sizeof positioned);
    assert(bs_bionic_pread(fd, positioned, 3, 2) == 3);
    assert(strcmp(positioned, "cXY") == 0);
    errno = 0;
    assert(bs_bionic_pread(fd, positioned, 1, -1) == -1 && errno == EINVAL);
    errno = 0;
    assert(bs_bionic_pwrite(0x4000, "X", 1, 0) == -1 && errno == EBADF);

    assert(bs_bionic_ftruncate(fd, 3) == 0);
    check_file_length(fd, 3);
    assert(lseek(fd, 0, SEEK_CUR) == 2);
    assert(bs_bionic_ftruncate(fd, 8192) == 0);
    check_file_length(fd, 8192);

    errno = 0;
    assert(bs_bionic_ftruncate(fd, -1) == -1 && errno == EINVAL);
    errno = 0;
    assert(bs_bionic_ftruncate(0x4000, 0) == -1 && errno == EBADF);
    assert(bs_bionic_fsync(fd) == 0);
    assert(bs_bionic_fdatasync(fd) == 0);
    errno = 0;
    assert(bs_bionic_fsync(-1) == -1 && errno == EBADF);
    errno = 0;
    assert(bs_bionic_fdatasync(0x4000) == -1 && errno == EBADF);

    errno = 0;
    assert(bs_bionic_flock(fd, 2) == -1 && errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_flock(fd, 1 | 4) == -1 && errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_flock(fd, 3) == -1 && errno == EINVAL);
    errno = 0;
    assert(bs_bionic_flock(-1, 2) == -1 && errno == EBADF);
    assert(close(fd) == 0);

    assert(bs_bionic_truncate("/guest/data", 5) == 0);
    fd = open(native_file, O_RDONLY);
    check_file_length(fd, 5);
    assert(close(fd) == 0);

    errno = 0;
    assert(bs_bionic_truncate("/forbidden/file", 0) == -1 && errno == ENOENT);
    errno = 0;
    assert(bs_bionic_truncate(NULL, 0) == -1 && errno == EFAULT);
}

static void test_volume_and_pathconf(void) {
    struct statvfs actual;
    assert(statvfs(native_file, &actual) == 0);
    BsBionicStatvfs result;
    memset(&result, 0xAA, sizeof result);
    assert(bs_bionic_statvfs("/guest/data", &result) == 0);
    assert(result.f_bsize == actual.f_bsize);
    assert(result.f_frsize == actual.f_frsize);
    assert(result.f_blocks == actual.f_blocks);
    assert(result.f_bfree == actual.f_bfree);
    assert(result.f_bavail == actual.f_bavail);
    assert(result.f_namemax == actual.f_namemax);
    for (size_t i = 0; i < 6; ++i) assert(result.reserved[i] == 0);

    BsBionicStatvfs before = result;
    errno = 0;
    assert(bs_bionic_statvfs("/guest/missing", &result) == -1);
    assert(errno == ENOENT && memcmp(&result, &before, sizeof result) == 0);
    errno = 0;
    assert(bs_bionic_statvfs("/guest/data", NULL) == -1 && errno == EFAULT);

    /* Android's _PC_NAME_MAX is 4; Linux's is 3. */
    assert(bs_bionic_pathconf("/guest/data", 4) ==
           pathconf(native_file, _PC_NAME_MAX));
    assert(bs_bionic_pathconf("/guest/data", 5) ==
           pathconf(native_file, _PC_PATH_MAX));
    errno = 0;
    assert(bs_bionic_pathconf("/guest/data", 999) == -1 && errno == EINVAL);
    errno = 0;
    assert(bs_bionic_pathconf("/guest/missing", _PC_NAME_MAX) == -1 && errno == ENOENT);
}

static void test_timestamps(void) {
    BsBionicTimeval t[2] = {{1000000000, 123456}, {1000000010, 654321}};
    assert(bs_bionic_utimes("/guest/data", t) == 0);
    struct stat st;
    assert(stat(native_file, &st) == 0);
    assert(st.st_mtim.tv_sec == t[1].tv_sec);
    assert(st.st_mtim.tv_nsec == t[1].tv_usec * 1000);

    BsBionicUtimbuf ut = {1000000020, 1000000030};
    assert(bs_bionic_utime("/guest/data", &ut) == 0);
    assert(stat(native_file, &st) == 0 && st.st_mtim.tv_sec == ut.modtime);

    int fd = open(native_file, O_RDWR);
    assert(fd >= 0);
    BsBionicTimespec ns[2] = {{1000000040, 111111111}, {1000000050, 222222222}};
    assert(bs_bionic_futimens(fd, ns) == 0);
    assert(stat(native_file, &st) == 0);
    assert(st.st_mtim.tv_sec == ns[1].tv_sec);
    assert(st.st_mtim.tv_nsec == ns[1].tv_nsec);

    ns[1].tv_sec = 1000000060;
    assert(bs_bionic_utimensat(-100, "/guest/data", ns, 0) == 0);
    assert(stat(native_file, &st) == 0 && st.st_mtim.tv_sec == ns[1].tv_sec);

    BsBionicTimespec invalid_ns[2] = {{1, 1000000000}, {1, 0}};
    errno = 0;
    assert(bs_bionic_futimens(fd, invalid_ns) == -1 && errno == EINVAL);
    errno = 0;
    assert(bs_bionic_utimensat(-100, "/guest/data", ns, 0x2000) == -1 && errno == EINVAL);
    errno = 0;
    assert(bs_bionic_utimensat(-100, "/guest/data", ns, 0x100) == -1 && errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_utimensat(fd, "data", ns, 0) == -1 && errno == ENOTSUP);

    BsBionicTimeval invalid_us[2] = {{1, -1}, {1, 0}};
    errno = 0;
    assert(bs_bionic_utimes("/guest/data", invalid_us) == -1 && errno == EINVAL);
    assert(close(fd) == 0);
}

static void test_process_metadata(void) {
    assert(bs_bionic_getuid() == (uint32_t)getuid());
    assert(bs_bionic_geteuid() == (uint32_t)geteuid());
    assert(bs_bionic_getgid() == (uint32_t)getgid());
    assert(bs_bionic_getegid() == (uint32_t)getegid());

    char actual[256], value[256];
    assert(gethostname(actual, sizeof actual) == 0);
    assert(bs_bionic_gethostname(value, sizeof value) == 0);
    assert(strcmp(actual, value) == 0);
    errno = 0;
    assert(bs_bionic_gethostname(NULL, sizeof value) == -1 && errno == EFAULT);

    BsBionicUtsname result;
    errno = 0;
    assert(bs_bionic_uname(&result) == -1 && errno == ENOSYS);
}

static void test_fdopendir(void) {
    char subdir[PATH_MAX], nested[PATH_MAX];
    assert(snprintf(subdir, sizeof subdir, "%s/directory", sandbox) <
           (int)sizeof subdir);
    assert(snprintf(nested, sizeof nested, "%s/directory/actual_entry", sandbox) <
           (int)sizeof nested);
    assert(mkdir(subdir, 0700) == 0);
    int child = open(nested, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(child >= 0 && close(child) == 0);

    /* Android path resolution registers the canonical native directory.
     * fdopendir opens an actual DIR and consumes the original descriptor. */
    int tracked = bs_android_open("/guest/directory", O_RDONLY);
    assert(tracked >= 0);
    DIR *converted = bs_bionic_fdopendir(tracked);
    assert(converted);
    int seen = 0;
    struct dirent *entry;
    while ((entry = readdir(converted)))
        if (strcmp(entry->d_name, "actual_entry") == 0) ++seen;
    assert(seen == 1);
    errno = 0;
    assert(fcntl(tracked, F_GETFD) == -1 && errno == EBADF);
    assert(closedir(converted) == 0);

    /* A duplicate retains a valid tracked directory while the source
     * descriptor stays open. Duplication of a non-directory clears tracking. */
    tracked = bs_android_open("/guest/directory", O_RDONLY);
    assert(tracked >= 0);
    int duplicate = dup(tracked);
    assert(duplicate >= 0);
    bs_bionic_note_dup2_fd(tracked, duplicate);
    converted = bs_bionic_fdopendir(duplicate);
    assert(converted && closedir(converted) == 0);
    assert(fcntl(duplicate, F_GETFD) == -1 && errno == EBADF);
    assert(fcntl(tracked, F_GETFD) >= 0);
    assert(close(tracked) == 0);

    /* A descriptor that was closed outside the asset bridge may be reused.
     * The file type / identity check must reject the stale directory record
     * while leaving the reused native descriptor owned by its caller. */
    tracked = bs_android_open("/guest/directory", O_RDONLY);
    assert(tracked >= 0 && close(tracked) == 0);
    int regular = open(native_file, O_RDONLY);
    assert(regular >= 0);
    if (regular != tracked) {
        assert(dup2(regular, tracked) == tracked);
        assert(close(regular) == 0);
        regular = tracked;
    }
    errno = 0;
    assert(bs_bionic_fdopendir(regular) == NULL && errno == ENOTDIR);
    assert(fcntl(regular, F_GETFD) >= 0);
    assert(close(regular) == 0);

    /* A failed conversion retains ownership for the caller. */
    regular = bs_android_open("/guest/data", O_RDONLY);
    assert(regular >= 0);
    errno = 0;
    assert(bs_bionic_fdopendir(regular) == NULL && errno == ENOTDIR);
    assert(fcntl(regular, F_GETFD) >= 0);
    assert(close(regular) == 0);

    /* Untracked real directory FDs use Linux's fdopendir fallback. */
    int fd = open(sandbox, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    DIR *dir = bs_bionic_fdopendir(fd);
    assert(dir);
    assert(readdir(dir));
    assert(closedir(dir) == 0);
    errno = 0;
    assert(bs_bionic_fdopendir(0x4000) == NULL && errno == EBADF);
    fd = open(native_file, O_RDONLY);
    assert(fd >= 0);
    errno = 0;
    assert(bs_bionic_fdopendir(fd) == NULL && errno == ENOTDIR);
    assert(close(fd) == 0);
    assert(unlink(nested) == 0);
    assert(rmdir(subdir) == 0);
}

int main(void) {
    char template_path[] = "/tmp/bs-bionic-posix-XXXXXX";
    char *temp = mkdtemp(template_path);
    assert(temp);
    assert(snprintf(sandbox, sizeof sandbox, "%s", temp) < (int)sizeof sandbox);
    assert(snprintf(native_file, sizeof native_file, "%s/data", sandbox) <
           (int)sizeof native_file);
    int fd = open(native_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);

    test_fd_operations();
    test_volume_and_pathconf();
    test_timestamps();
    test_process_metadata();
    test_fdopendir();

    assert(unlink(native_file) == 0);
    assert(rmdir(sandbox) == 0);
    puts("bionic_posix_bridge host tests passed");
    return 0;
}
