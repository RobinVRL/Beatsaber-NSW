#ifndef BEAT_SABER_ASSET_FS_BRIDGE_H
#define BEAT_SABER_ASSET_FS_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <wchar.h>

/* Private Quest APK/OBB files staged under sdmc:/switch/beat-saber-quest/.
 * Android Unix paths must be mapped to that tree, and bionic stat/dirent
 * layouts translated rather than passed directly to newlib.
 */
int bs_android_open(const char *path, int flags, ...);
int bs_android_openat(int dirfd, const char *path, int flags, ...);
int bs_android_stat(const char *path, void *out);
int bs_android_fstat(int fd, void *out);
int bs_android_lstat(const char *path, void *out);
int bs_android_mkdir(const char *path, unsigned mode);
int bs_android_access(const char *path, int mode);
int bs_android_chmod(const char *path, unsigned mode);
int bs_android_fchmod(int fd, unsigned mode);
int bs_android_fchmodat(int dirfd, const char *path, unsigned mode, int flags);
int bs_android_unlinkat(int dirfd, const char *path, int flags);
int bs_android_unlink(const char *path);
int bs_android_rmdir(const char *path);
int bs_android_remove(const char *path);
int bs_android_rename(const char *oldpath, const char *newpath);
int bs_android_renameat(int olddirfd, const char *oldpath, int newdirfd,
                        const char *newpath);
int bs_android_link(const char *oldpath, const char *newpath);
int bs_android_symlink(const char *target, const char *linkpath);
ssize_t bs_android_readlink(const char *path, char *buffer, size_t capacity);
ssize_t bs_android_sendfile(int out_fd, int in_fd, int64_t *offset,
                            size_t count);
int bs_android_dup2(int oldfd, int newfd);
FILE *bs_android_fopen(const char *path, const char *mode);
void *bs_android_opendir(const char *path);
void *bs_android_readdir(void *dir);
char *bs_android_realpath(const char *path, char *resolved);
char *bs_android_getcwd(char *buffer, size_t capacity);
char *bs_android_getenv(const char *key);
int bs_android_chdir(const char *path);

/* Standard-stream adapters (Bionic __sF is not newlib FILE). */
extern unsigned char bs_bionic_streams[3][256];
extern FILE *bs_bionic_stdin;
extern FILE *bs_bionic_stdout;
extern FILE *bs_bionic_stderr;
size_t bs_android_fwrite(const void *p, size_t size, size_t count, FILE *f);
size_t bs_android_fread(void *p, size_t size, size_t count, FILE *f);
int bs_android_fclose(FILE *f);
int bs_android_fflush(FILE *f);
int bs_android_fprintf(FILE *f, const char *format, ...);
int bs_android_vfprintf(FILE *f, const char *format, va_list args);
int bs_android_fputs(const char *message, FILE *f);
int bs_android_fputc(int ch, FILE *f);
int bs_android_ferror(FILE *f);
int bs_android_feof(FILE *f);
int bs_android_fileno(FILE *f);
int bs_android_fseek(FILE *f, long off, int whence);
long bs_android_ftell(FILE *f);
int bs_android_fscanf(FILE *f, const char *format, ...);
int bs_android_vfscanf(FILE *f, const char *format, va_list args);
char *bs_android_fgets(char *buffer, int capacity, FILE *stream);
FILE *bs_android_fdopen(int fd, const char *mode);
void bs_android_setbuf(FILE *stream, char *buffer);
void bs_android_clearerr(FILE *stream);
void bs_android_perror(const char *label);
int bs_android_fseeko(FILE *f, off_t offset, int origin);
off_t bs_android_ftello(FILE *f);
int bs_android_getc(FILE *f);
int bs_android_ungetc(int ch, FILE *f);
wint_t bs_android_getwc(FILE *f);
wint_t bs_android_ungetwc(wint_t ch, FILE *f);
wint_t bs_android_fputwc(wchar_t ch, FILE *f);

/* NDK assets and GameActivity configuration. */
void *bs_android_assetmanager_from_java(void *env, void *jobject);
void *bs_android_assetmanager_open(void *manager, const char *name, int mode);
void *bs_android_assetmanager_open_dir(void *manager, const char *name);
const char *bs_android_asset_dir_get_next_file_name(void *dir);
void bs_android_asset_dir_rewind(void *dir);
void bs_android_asset_dir_close(void *dir);
void bs_android_asset_close(void *asset);
int bs_android_asset_read(void *asset, void *buffer, size_t count);
int64_t bs_android_asset_seek64(void *asset, int64_t offset, int whence);
int64_t bs_android_asset_length64(void *asset);
int64_t bs_android_asset_length(void *asset);
const void *bs_android_asset_get_buffer(void *asset);
void *bs_android_configuration_new(void);
void bs_android_configuration_delete(void *config);
void bs_android_configuration_from_assetmanager(void *config, void *manager);
void bs_android_configuration_get_language(void *config, char *out);
void bs_android_configuration_get_country(void *config, char *out);
int bs_android_configuration_get_sdk_version(void *config);

#endif
