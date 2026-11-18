/* Minimal ABI-compatible Bionic process/libc interfaces used by Quest ELF.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_BIONIC_MISC_BRIDGE_H
#define BS_BIONIC_MISC_BRIDGE_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
void *bs_bionic_memrchr(const void *buffer, int value, size_t length);
char *bs_bionic_strcasestr(const char *haystack, const char *needle);
char *bs_bionic_basename(char *path);
char *bs_bionic_dirname(char *path);
int bs_bionic_vasprintf(char **out, const char *format, va_list args);
void bs_bionic_openlog(const char *ident, int option, int facility);
void bs_bionic_syslog(int priority, const char *format, ...);
void bs_bionic_closelog(void);
int bs_bionic_raise(int signal_number);
/* Additional real Bionic/NDK exports resolved by the ELF loader. */
uintptr_t bs_bionic_extra_symbol(const char *name);
#endif
