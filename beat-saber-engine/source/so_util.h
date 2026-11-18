/* so_util.h -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <elf.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define SO_MAX_SEGMENTS 8

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;
  char name[64];

  // entire LOAD zone
  void *load_base, *load_virtbase;
  size_t load_size;
  void *load_memrv; // VirtmemReservation *

  // copy of the unmodified program headers (link-time vaddrs),
  // needed for dl_iterate_phdr / exception unwinding
  Elf64_Phdr phdr[SO_MAX_SEGMENTS * 2];
  int phnum;

  // temporary file image
  void *so_base;
  size_t so_size;

  Elf64_Ehdr *elf_hdr;
  Elf64_Phdr *prog_hdr;
  Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms;
  int num_syms;
  char *shstrtab;
  char *dynstrtab;
  Elf64_Rela *packed_rela;
  size_t packed_rela_count;
  /* Persistent constructor metadata; section headers live in so_base, which
   * is released once relocation/finalization has finished. */
  uintptr_t init_array_vaddr;
  size_t init_array_count;
} so_module;

void hook_arm64(uintptr_t addr, uintptr_t dst);

void so_flush_caches(so_module *mod);
void so_free_temp(so_module *mod);
int so_load(so_module *mod, const char *filename, void *base, size_t max_size);
int so_relocate(so_module *mod);
int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
void so_execute_init_array(so_module *mod, bool (*should_stop)(void));
uintptr_t so_find_addr(so_module *mod, const char *symbol);
uintptr_t so_find_addr_rx(so_module *mod, const char *symbol);
// returns 0 instead of aborting when the symbol is missing
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name);
// dlsym() backing: search every loaded module's exports for `name`.
void *so_resolve_external(const char *name);
/* Look up a loaded Android shared object by basename or its full SD path. */
so_module *so_find_module_by_name(const char *name);
/* Validate that a caller's dlopen handle is an actual loaded module pointer. */
int so_is_loaded_module(const so_module *module);
/* Return the real runtime address of a defined, exported ELF symbol. */
uintptr_t so_find_module_export(so_module *mod, const char *name);
// Walk the loaded-module list; return the module whose mapped LOAD zone
// contains `addr`, or NULL. Used by the diagnostic backtrace to symbolicate.
so_module *so_find_module_by_addr(const void *addr);
int so_dump_maps(char *buf, size_t cap);
uint32_t so_finalize(so_module *mod);
void so_patch_stack_canaries(so_module *mod);
int so_unload(so_module *mod);

// dl_iterate_phdr() replacement operating on all loaded modules;
// required by the libunwind embedded in libc++_shared.so
int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

int so_patch_code(void *dst, const void *src, size_t len);

/* Switch 2 (kaiHEN): modules live in one reserved arena of the stack region,
 * text mapped as code by RO and data aliased with svcMapMemory. Reports the
 * arena so the mmap window avoids it and bounces file I/O on its data pages.
 * Always false on Switch 1. */
bool so_arena_range(uintptr_t *start, uintptr_t *end);

#endif
