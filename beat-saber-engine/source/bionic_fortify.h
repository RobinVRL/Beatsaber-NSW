/* Android Bionic fortified libc entrypoints used by the original Quest ELFs.
 * They enforce the same buffer-size checks before delegating to the existing
 * Android filesystem/stdio bridge. Copyright (c) 2026, MIT.
 */
#ifndef BEAT_SABER_BIONIC_FORTIFY_H
#define BEAT_SABER_BIONIC_FORTIFY_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>

void *bs_fortify_memcpy(void *dst, const void *src, size_t bytes, size_t dst_bytes);
void *bs_fortify_memmove(void *dst, const void *src, size_t bytes, size_t dst_bytes);
void *bs_fortify_memset(void *dst, int ch, size_t bytes, size_t dst_bytes);
size_t bs_fortify_strlen(const char *str, size_t max_bytes);
char *bs_fortify_strchr(const char *str, int ch, size_t max_bytes);
char *bs_fortify_strncat(char *dst, const char *src, size_t bytes, size_t dst_bytes);
int bs_fortify_vsnprintf(char *dst, size_t bytes, int flags, size_t dst_bytes,
                         const char *format, va_list args);
ssize_t bs_fortify_write(int fd, const void *src, size_t bytes, size_t src_bytes);
ssize_t bs_fortify_pwrite(int fd, const void *src, size_t bytes, off_t offset,
                          size_t src_bytes);
size_t bs_fortify_fwrite(const void *src, size_t size, size_t count, FILE *stream,
                         size_t src_bytes);
int bs_fortify_open2(const char *path, int flags);
#endif
