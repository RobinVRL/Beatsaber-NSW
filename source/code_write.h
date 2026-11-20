#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

void bs_code_write_init(void);
/* Exception-handler hook; true = store emulated, resume at ctx->pc. */
bool bs_code_write_fault(ThreadExceptionDump *ctx);
int bs_code_write(void *dst, const void *src, size_t len);
unsigned long bs_code_write_count(void);
void *bs_exec_alloc(size_t size);
bool bs_exec_owns(const void *p, size_t len);
/* 0 = handled, 1 = not a code page, -1 = error. */
int bs_code_mprotect(void *addr, size_t len, int prot_exec);
bool bs_code_write_selftest(void);
