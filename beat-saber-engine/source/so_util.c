/* so_util.c -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Loads Android ARM64 shared objects into a reserved memory range and applies
 * the relocation types used by libff4.so.
 */

#include <switch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <elf.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"
#include "guest_tls.h"
#include "elf_symbol_versions.h"
#include "bionic_misc_bridge.h"
#include "bionic_event_epoll.h"

// not in devkitA64's elf.h
#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003
#ifndef SHT_ANDROID_RELA
#define SHT_ANDROID_RELA 0x60000002
#endif
#ifndef R_AARCH64_TLSDESC
#define R_AARCH64_TLSDESC 1031
#endif

static so_module *so_list = NULL;

static Handle code_process_handle(void) {
  Handle process = envGetOwnProcessHandle();
  return process == INVALID_HANDLE ? CUR_PROCESS_HANDLE : process;
}

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  uint32_t *hook = (uint32_t *)addr;
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

void so_flush_caches(so_module *mod) {
  armDCacheFlush(mod->load_virtbase, mod->load_size);
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
}

void so_free_temp(so_module *mod) {
  free(mod->packed_rela);
  mod->packed_rela = NULL;
  mod->packed_rela_count = 0;
  free(mod->so_base);
  mod->so_base = NULL;
  // These pointers reference so_base and must not survive file-image cleanup.
  // The exported dynsym/dynstr pointers have already been rebased in finalize.
  mod->elf_hdr = NULL;
  mod->prog_hdr = NULL;
  mod->sec_hdr = NULL;
  mod->shstrtab = NULL;
}

uint32_t so_finalize(so_module *mod) {
  // the kernel never allows W -> X transitions on code memory, so the RX
  // segments must be set first, straight from the freshly-mapped state;
  // everything else (data segments AND the alignment gaps lld leaves
  // between segments) becomes RW afterwards. build a page map to emit
  // each permission exactly once per page.
  const size_t num_pages = mod->load_size / 0x1000;
  uint8_t *is_x_page = calloc(num_pages, 1);
  if (!is_x_page)
    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    const size_t first = p->p_vaddr / 0x1000;
    const size_t last = (ALIGN_MEM(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000) - 1;
    for (size_t pg = first; pg <= last && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  Handle process = code_process_handle();
  const u64 runtime_address = (u64)mod->load_virtbase;
  u64 staged_address = (u64)mod->load_base;
  Result rc = svcMapProcessCodeMemory(process, runtime_address, staged_address,
                                     mod->load_size);
  for (int attempt = 0; R_FAILED(rc) && attempt < 3; ++attempt) {
    // The source must be plain heap (MemType_Heap, RW, no attributes). Heap
    // pages that a driver or IPC shared (DeviceShared/IpcLocked) are refused
    // with InvalidCurrentMemory. Report the first bad page, then retry from a
    // fresh copy. The old buffer is deliberately leaked so the allocator can't
    // hand the same pages out again.
    for (u64 a = staged_address; a < staged_address + mod->load_size;) {
      MemoryInfo info; u32 page;
      if (R_FAILED(svcQueryMemory(&info, &page, a))) break;
      if (info.type != MemType_Heap || info.attr || info.perm != Perm_Rw) {
        debugPrintf("[link] %s: staged page %p type=0x%x attr=0x%x perm=0x%x "
                    "(block %p+%#llx)\n", mod->name, (void *)a, info.type,
                    info.attr, info.perm, (void *)info.addr,
                    (unsigned long long)info.size);
        break;
      }
      a = info.addr + info.size;
    }
    void *fresh = memalign(0x1000, mod->load_size);
    if (!fresh) break;
    memcpy(fresh, mod->load_base, mod->load_size);
    const intptr_t move = (intptr_t)fresh - (intptr_t)mod->load_base;
    if (mod->syms)      mod->syms      = (Elf64_Sym *)((uintptr_t)mod->syms + move);
    if (mod->dynstrtab) mod->dynstrtab = (char *)((uintptr_t)mod->dynstrtab + move);
    mod->load_base = fresh;
    staged_address = (u64)fresh;
    rc = svcMapProcessCodeMemory(process, runtime_address, staged_address,
                                 mod->load_size);
    debugPrintf("[link] %s: retry %d from fresh staging %p -> 0x%x\n",
                mod->name, attempt + 1, fresh, rc);
  }
  if (R_FAILED(rc)) {
    debugPrintf("[link] %s: svcMapProcessCodeMemory failed (handle=0x%x, result=0x%x)\n",
                mod->name, process, rc);
    free(is_x_page);
    return rc;
  }

  // pass 0 sets the executable runs to RX, pass 1 sets the rest to RW
  for (int want_x = 1; want_x >= 0; want_x--) {
    size_t pg = 0;
    while (pg < num_pages) {
      if (is_x_page[pg] != want_x) {
        pg++;
        continue;
      }
      size_t run_end = pg;
      while (run_end < num_pages && is_x_page[run_end] == want_x)
        run_end++;
      const u64 addr = (u64)mod->load_virtbase + pg * 0x1000;
      const u64 size = (run_end - pg) * 0x1000;
      rc = svcSetProcessMemoryPermission(process, addr, size, want_x ? Perm_Rx : Perm_Rw);
      if (R_FAILED(rc)) {
        debugPrintf("[link] %s: setting %s permission at %p failed (result=0x%x)\n",
                    mod->name, want_x ? "RX" : "RW", (void *)addr, rc);
        Result cleanup = svcUnmapProcessCodeMemory(process, runtime_address,
                                                   staged_address, mod->load_size);
        if (R_FAILED(cleanup))
          debugPrintf("[link] %s: unmapping after failure returned 0x%x\n",
                      mod->name, cleanup);
        free(is_x_page);
        return rc;
      }
      pg = run_end;
    }
  }

  free(is_x_page);

  // svcMapProcessCodeMemory donates the source pages (load_base) to the code
  // mapping; afterwards load_base is in CodeMemory state and faults on access --
  // the image is readable only via load_virtbase. mod->syms/dynstrtab were set to
  // load_base for pre-finalize import resolution (when load_virtbase isn't mapped
  // yet). Rebase them to load_virtbase now so post-finalize lookups (dlsym ->
  // so_lookup_export, e.g. libunity resolving il2cpp_init) read the live mapped
  // .dynsym/.dynstr instead of faulting on the donated source range.
  const uintptr_t delta = (uintptr_t)mod->load_virtbase - (uintptr_t)mod->load_base;
  if (mod->syms)      mod->syms      = (Elf64_Sym *)((uintptr_t)mod->syms + delta);
  if (mod->dynstrtab) mod->dynstrtab = (char *)((uintptr_t)mod->dynstrtab + delta);
  return 0;
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;

  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);

  FILE *fd = fopen(filename, "rb");
  if (fd == NULL)
    return -1;

  if (fseek(fd, 0, SEEK_END)) {
    fclose(fd);
    return -1;
  }
  long filesize = ftell(fd);
  if (filesize <= 0 || fseek(fd, 0, SEEK_SET)) {
    fclose(fd);
    return -1;
  }
  mod->so_size = (size_t)filesize;

  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) {
    fclose(fd);
    return -2;
  }

  size_t bytes_read = fread(mod->so_base, 1, mod->so_size, fd);
  fclose(fd);
  if (bytes_read != mod->so_size || mod->so_size < sizeof(Elf64_Ehdr)) {
    res = -1;
    goto err_free_so;
  }

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_so;
  }

  mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
  if (mod->elf_hdr->e_phentsize != sizeof(Elf64_Phdr) ||
      mod->elf_hdr->e_shentsize != sizeof(Elf64_Shdr) ||
      mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2 ||
      mod->elf_hdr->e_shnum == 0 ||
      mod->elf_hdr->e_shstrndx >= mod->elf_hdr->e_shnum ||
      mod->elf_hdr->e_phoff > mod->so_size ||
      mod->elf_hdr->e_phnum > (mod->so_size - mod->elf_hdr->e_phoff) / sizeof(Elf64_Phdr) ||
      mod->elf_hdr->e_shoff > mod->so_size ||
      mod->elf_hdr->e_shnum > (mod->so_size - mod->elf_hdr->e_shoff) / sizeof(Elf64_Shdr)) {
    res = -4;
    goto err_free_so;
  }
  mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  const Elf64_Shdr *names = &mod->sec_hdr[mod->elf_hdr->e_shstrndx];
  if (names->sh_offset > mod->so_size ||
      names->sh_size > mod->so_size - names->sh_offset) {
    res = -4;
    goto err_free_so;
  }
  mod->shstrtab = (char *)mod->so_base + names->sh_offset;

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("so_load: %s has too many program headers (%d)\n", filename, mod->elf_hdr->e_phnum);
    res = -4;
    goto err_free_so;
  }

  // save a pristine copy of the phdrs for dl_iterate_phdr
  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  // total size of the LOAD zone = highest vaddr+memsz of all LOAD segments
  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }

  // align total size to page size
  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  mod->load_base = base;
  if (!mod->load_base) goto err_free_so;
  memset(mod->load_base, 0, mod->load_size);

  // reserve virtual memory space for the entire LOAD zone
  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
  mod->load_memrv = mod->load_virtbase
                      ? virtmemAddReservation(mod->load_virtbase, mod->load_size)
                      : NULL;
  virtmemUnlock();
  if (!mod->load_virtbase || !mod->load_memrv) {
    debugPrintf("so_load: no available code memory mapping for %s (%zu bytes)\n",
                filename, mod->load_size);
    res = -5;
    goto err_free_so;
  }

  // copy segments to where they belong, then fix up the runtime phdrs to
  // point into the virtual address space
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset),
             p->p_filesz);
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const Elf64_Shdr *section = &mod->sec_hdr[i];
    if (section->sh_name >= names->sh_size ||
        !memchr(mod->shstrtab + section->sh_name, 0,
                names->sh_size - section->sh_name)) {
      res = -6;
      goto err_free_load;
    }
    char *sh_name = mod->shstrtab + section->sh_name;
    if ((!strcmp(sh_name, ".dynsym") || !strcmp(sh_name, ".dynstr")) &&
        (section->sh_addr > mod->load_size ||
         section->sh_size > mod->load_size - section->sh_addr)) {
      res = -6;
      goto err_free_load;
    }
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    } else if (strcmp(sh_name, ".init_array") == 0) {
      /* Cache section location before so_free_temp invalidates sec_hdr. */
      if (mod->sec_hdr[i].sh_addr > mod->load_size ||
          mod->sec_hdr[i].sh_size > mod->load_size - mod->sec_hdr[i].sh_addr ||
          mod->sec_hdr[i].sh_size % sizeof(uintptr_t)) {
        res = -6;
        goto err_free_load;
      }
      mod->init_array_vaddr = (uintptr_t)mod->sec_hdr[i].sh_addr;
      mod->init_array_count = mod->sec_hdr[i].sh_size / sizeof(uintptr_t);
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) {
    res = -2;
    goto err_free_load;
  }

  // register in the global module list (append, load order matters for resolution)
  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next) m = m->next;
    m->next = mod;
  }

  return 0;

err_free_load:
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;

  return res;
}

// find a dynamic tag in the file image; returns 0 if missing
static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      if (mod->phdr[i].p_offset > mod->so_size ||
          mod->phdr[i].p_filesz > mod->so_size - mod->phdr[i].p_offset)
        return 0;
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base + mod->phdr[i].p_offset);
      size_t count = mod->phdr[i].p_filesz / sizeof(*dyn);
      for (size_t j = 0; j < count && dyn[j].d_tag != DT_NULL; j++)
        if (dyn[j].d_tag == tag)
          return dyn[j].d_un.d_val;
    }
  }
  return 0;
}

// RELR/ANDROID_RELR packed relative relocations;
// each location gets the load base added to the value stored in the file
static bool so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  uintptr_t where = 0;
  if (relrsz % sizeof(Elf64_Xword)) return false;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      // even entry: address of the next relocation
      where = (uintptr_t)entry;
      if ((where & 7) || where > mod->load_size ||
          mod->load_size - where < sizeof(uint64_t)) return false;
      *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
      where += 8;
    } else {
      // odd entry: bitmap for the next 63 words
      for (int bit = 1; bit < 64; bit++) {
        if (entry & (1ull << bit)) {
          const size_t offset = (size_t)(bit - 1) * 8;
          if (where > mod->load_size ||
              offset > mod->load_size - where ||
              mod->load_size - where - offset < sizeof(uint64_t)) return false;
          *(uint64_t *)((uintptr_t)mod->load_base + where + offset) += (uint64_t)mod->load_virtbase;
        }
      }
      if (where > SIZE_MAX - 63 * 8) return false;
      where += 63 * 8;
    }
  }
  return true;
}

static bool read_sleb128(const uint8_t **cursor, const uint8_t *end,
                         uint64_t *value) {
  *value = 0;
  for (unsigned shift = 0; shift < 64 && *cursor < end; shift += 7) {
    uint8_t byte = *(*cursor)++;
    if (shift == 63 && byte != 0 && byte != 0x7f)
      return false;
    *value |= (uint64_t)(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      if ((byte & 0x40) && shift < 57)
        *value |= UINT64_MAX << (shift + 7);
      return true;
    }
  }
  return false;
}

/* Android APS2 encodes consecutive RELA entries as groups of signed deltas. */
static bool decode_android_rela(so_module *mod, const Elf64_Shdr *section) {
  enum { GROUP_INFO = 1, GROUP_OFFSET = 2, GROUP_ADDEND = 4, HAS_ADDEND = 8 };
  if (mod->packed_rela || section->sh_offset > mod->so_size ||
      section->sh_size > mod->so_size - section->sh_offset ||
      section->sh_size < 4)
    return false;
  const uint8_t *cursor = (const uint8_t *)mod->so_base + section->sh_offset;
  const uint8_t *end = cursor + section->sh_size;
  if (memcmp(cursor, "APS2", 4) != 0)
    return false;
  cursor += 4;

  uint64_t count, offset;
  if (!read_sleb128(&cursor, end, &count) ||
      !read_sleb128(&cursor, end, &offset) ||
      count > mod->load_size / sizeof(uint64_t) ||
      count > SIZE_MAX / sizeof(Elf64_Rela))
    return false;
  Elf64_Rela *relocs = calloc(count ? count : 1, sizeof(*relocs));
  if (!relocs)
    return false;

  uint64_t addend = 0;
  size_t index = 0;
  while (index < count) {
    uint64_t group, flags, delta = 0, info = 0, addend_delta;
    if (!read_sleb128(&cursor, end, &group) || group == 0 || group > count - index ||
        !read_sleb128(&cursor, end, &flags) || flags > 15 ||
        ((flags & GROUP_ADDEND) && !(flags & HAS_ADDEND)))
      goto invalid;
    if ((flags & GROUP_OFFSET) && !read_sleb128(&cursor, end, &delta))
      goto invalid;
    if ((flags & GROUP_INFO) && !read_sleb128(&cursor, end, &info))
      goto invalid;
    if (flags & GROUP_ADDEND) {
      if (!read_sleb128(&cursor, end, &addend_delta))
        goto invalid;
      addend += addend_delta;
    } else if (!(flags & HAS_ADDEND)) {
      addend = 0;
    }

    for (uint64_t entry = 0; entry < group; entry++) {
      if (!(flags & GROUP_OFFSET) && !read_sleb128(&cursor, end, &delta))
        goto invalid;
      if (!(flags & GROUP_INFO) && !read_sleb128(&cursor, end, &info))
        goto invalid;
      if ((flags & HAS_ADDEND) && !(flags & GROUP_ADDEND)) {
        if (!read_sleb128(&cursor, end, &addend_delta))
          goto invalid;
        addend += addend_delta;
      }
      offset += delta;
      if (offset > mod->load_size - sizeof(uint64_t) ||
          ELF64_R_SYM(info) >= (size_t)mod->num_syms)
        goto invalid;
      relocs[index++] = (Elf64_Rela){ offset, info, (Elf64_Sxword)addend };
    }
  }
  if (cursor != end)
    goto invalid;
  mod->packed_rela = relocs;
  mod->packed_rela_count = index;
  debugPrintf("[link] decoded %zu Android APS2 relocations in %s\n", index, mod->name);
  return true;

invalid:
  free(relocs);
  return false;
}

static const Elf64_Rela *relocation_entries(const so_module *mod, int index,
                                           size_t *count) {
  const Elf64_Shdr *section = &mod->sec_hdr[index];
  const char *name = mod->shstrtab + section->sh_name;
  if (section->sh_type == SHT_ANDROID_RELA) {
    *count = mod->packed_rela_count;
    return mod->packed_rela;
  }
  if (section->sh_type == SHT_RELA &&
      (strcmp(name, ".rela.dyn") == 0 || strcmp(name, ".rela.plt") == 0)) {
    *count = section->sh_size / sizeof(Elf64_Rela);
    return (const Elf64_Rela *)((uintptr_t)mod->load_base + section->sh_addr);
  }
  *count = 0;
  return NULL;
}

int so_relocate(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    if (mod->sec_hdr[i].sh_type == SHT_ANDROID_RELA &&
        !decode_android_rela(mod, &mod->sec_hdr[i]))
      return -1;
    size_t count;
    const Elf64_Rela *rels = relocation_entries(mod, i, &count);
    if (rels) {
      for (size_t j = 0; j < count; j++) {
        if (rels[j].r_offset > mod->load_size ||
            mod->load_size - rels[j].r_offset < sizeof(uintptr_t) ||
            ELF64_R_SYM(rels[j].r_info) >= (size_t)mod->num_syms) {
          debugPrintf("[link] %s: out-of-bounds RELA entry %zu\n", mod->name, j);
          return -1;
        }
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
            if (sym->st_shndx == SHN_UNDEF) {
              // Imported absolute relocations (notably RTTI/type_info vtable
              // references) must be resolved after all modules are loaded.
              *ptr = rels[j].r_addend;
            } else {
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            }
            break;

          case R_AARCH64_RELATIVE:
            // sometimes the value of r_addend is also at *ptr
            *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
            break;

          case R_AARCH64_TLSDESC:
            /* The actual Oculus and OpenXR modules use local STN_UNDEF TLS
             * descriptors with a RELA addend into their own PT_TLS image.
             * Reject unknown external TLS and invalid descriptor positions.
             */
            if (!bs_guest_tls_apply_desc(mod, &rels[j])) {
              debugPrintf("[tls] %s: TLSDESC relocation rejected at 0x%lx\n",
                          mod->name, (unsigned long)rels[j].r_offset);
              return -1;
            }
            break;

          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx != SHN_UNDEF)
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            break;
          }

          default:
            debugPrintf("[link] %s: unsupported relocation type 0x%x\n",
                        mod->name, type);
            return -1;
        }
      }
    }
  }

  // packed relative relocations, if present (lld emits these for newer libs)
  Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }
  if (relr_off && relr_size) {
    if (relr_off > mod->load_size ||
        relr_size > mod->load_size - relr_off ||
        !so_process_relr(mod,
            (const Elf64_Xword *)((uintptr_t)mod->load_base + relr_off), relr_size)) {
      debugPrintf("[link] %s: invalid/out-of-bounds RELR relocation table\n", mod->name);
      return -1;
    }
  }


  return 0;
}

/* After so_free_temp, export tables remain in the mapped PT_LOAD pages but
 * their backing file images are gone. Recover DT_VERSYM from the mapped
 * PT_DYNAMIC (using the preserved *unrebased* phdr copies) so post-finalize
 * dlsym also refuses GNU non-default/hidden versions.
 */
static const Elf64_Half *so_runtime_versym(const so_module *mod) {
  if (!mod || !mod->load_virtbase || mod->num_syms <= 0 ||
      (size_t)mod->num_syms > mod->load_size / sizeof(Elf64_Half))
    return NULL;
  for (int i = 0; i < mod->phnum; ++i) {
    const Elf64_Phdr *ph = &mod->phdr[i];
    if (ph->p_type != PT_DYNAMIC || ph->p_vaddr > mod->load_size ||
        ph->p_memsz > mod->load_size - ph->p_vaddr)
      continue;
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)(
        (uintptr_t)mod->load_virtbase + ph->p_vaddr);
    for (size_t j = 0; j < ph->p_memsz / sizeof(*dyn); ++j) {
      if (dyn[j].d_tag == DT_NULL) break;
      if (dyn[j].d_tag != 0x6ffffff0 /* DT_VERSYM */) continue;
      uintptr_t off = (uintptr_t)dyn[j].d_un.d_ptr;
      if (off > mod->load_size ||
          (size_t)mod->num_syms * sizeof(Elf64_Half) > mod->load_size - off)
        return NULL;
      return (const Elf64_Half *)((uintptr_t)mod->load_virtbase + off);
    }
  }
  return NULL;
}

// Find a public, defined export in one module. Before finalize use the
// bounded version parser; afterwards use the validated mapped dynsym/versym.
static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  if (!mod || !name || !*name) return 0;
  if (mod->so_base) {
    BsElfVersionView view;
    BsElfVersionExport selected;
    if (bs_elf_version_view_init_rebased(&view, mod->so_base, mod->so_size,
                (uintptr_t)mod->load_virtbase) != BS_ELF_VERSION_MATCH ||
        bs_elf_version_find_export(&view, mod->name, name, NULL, &selected)
                != BS_ELF_VERSION_MATCH)
      return 0;
    return selected.absolute ? (uintptr_t)selected.symbol_value :
           (uintptr_t)mod->load_virtbase + selected.symbol_value;
  }
  const Elf64_Half *versions = so_runtime_versym(mod);
  for (int i = 0; i < mod->num_syms; i++) {
    Elf64_Sym *sym = &mod->syms[i];
    unsigned bind = ELF64_ST_BIND(sym->st_info);
    unsigned kind = ELF64_ST_TYPE(sym->st_info);
    unsigned visibility = ELF64_ST_VISIBILITY(sym->st_other);
    if (sym->st_shndx == SHN_UNDEF ||
        (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_GNU_UNIQUE) ||
        (visibility != STV_DEFAULT && visibility != STV_PROTECTED) ||
        kind == STT_TLS || kind == 10 /* STT_GNU_IFUNC */)
      continue;
    if (versions && ((versions[i] & 0x8000u) || !(versions[i] & 0x7fffu)))
      continue;
    if (sym->st_shndx != SHN_ABS &&
        (sym->st_value >= mod->load_size ||
         sym->st_size > mod->load_size - sym->st_value))
      continue;
    const char *sname = mod->dynstrtab + sym->st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return sym->st_shndx == SHN_ABS ? (uintptr_t)sym->st_value :
             (uintptr_t)mod->load_virtbase + sym->st_value;
  }
  return 0;
}

static const char *so_basename(const char *path) {
  if (!path) return NULL;
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

so_module *so_find_module_by_name(const char *name) {
  const char *base = so_basename(name);
  if (!base || !*base) return NULL;
  for (so_module *m = so_list; m; m = m->next) {
    if (!strcmp(m->name, name) || !strcmp(so_basename(m->name), base))
      return m;
  }
  return NULL;
}

int so_is_loaded_module(const so_module *module) {
  if (!module) return 0;
  for (so_module *m = so_list; m; m = m->next)
    if (m == module) return 1;
  return 0;
}

uintptr_t so_find_module_export(so_module *mod, const char *symbol) {
  if (!mod || !symbol || !*symbol) return 0;
  return so_lookup_export(mod, symbol);
}

// dlsym() backing: search every loaded module's exports for `name`.
void *so_resolve_external(const char *name) {
  if (!name)
    return NULL;
  for (so_module *m = so_list; m; m = m->next) {
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return (void *)addr;
  }
  return NULL;
}

so_module *so_find_module_by_addr(const void *addr) {
  const uintptr_t a = (uintptr_t)addr;
  for (so_module *m = so_list; m; m = m->next) {
    const uintptr_t base = (uintptr_t)m->load_virtbase;
    if (a >= base && a < base + m->load_size)
      return m;
  }
  return NULL;
}

// Emit /proc/self/maps-style segment records with the genuine ELF PT_LOAD
// permissions. The initial loader reported each entire module as RX, which
// hid the game's actual RW global data segments from libil2cpp's conservative
// GC dynamic-library root scanner. Accurate segments matter after startup.
// Returns the number of bytes written (excluding the NUL).
int so_dump_maps(char *buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  size_t off = 0;
  for (so_module *m = so_list; m; m = m->next) {
    for (int j = 0; j < m->phnum; ++j) {
      const Elf64_Phdr *p = &m->phdr[j];
      if (p->p_type != PT_LOAD || !p->p_memsz) continue;
      uintptr_t s = (uintptr_t)m->load_virtbase + p->p_vaddr;
      uintptr_t e = s + p->p_memsz;
      int n = snprintf(buf + off, cap - off,
                       "%012lx-%012lx %c%c%cp %08lx 00:00 0          %s\n",
                       (unsigned long)s, (unsigned long)e,
                       (p->p_flags & PF_R) ? 'r' : '-',
                       (p->p_flags & PF_W) ? 'w' : '-',
                       (p->p_flags & PF_X) ? 'x' : '-',
                       (unsigned long)p->p_offset,
                       m->name[0] ? m->name : "module");
      if (n < 0 || (size_t)n >= cap - off) {
        buf[off < cap ? off : cap - 1] = '\0';
        return (int)off;
      }
      off += (size_t)n;
    }
  }
  buf[off < cap ? off : cap - 1] = '\0';
  return (int)off;
}

/* Inspect the original bounded ELF file image, not arbitrary dynsym pointers.
 * The provider's declared SONAME is authoritative over its staging filename;
 * this supports aliases without ever binding to an unrelated same-name ELF.
 */
static int so_versioned_provider(so_module *provider, const char *needed,
                                 const char *symbol,
                                 const BsElfVersionRequirement *requirement,
                                 uintptr_t *address) {
  if (!provider || !provider->so_base) return BS_ELF_VERSION_INVALID;
  BsElfVersionView view;
  if (bs_elf_version_view_init_rebased(&view, provider->so_base,
                                       provider->so_size,
                                       (uintptr_t)provider->load_virtbase) !=
      BS_ELF_VERSION_MATCH)
    return BS_ELF_VERSION_INVALID;
  if (needed) {
    const char *identity = view.soname ? view.soname : so_basename(provider->name);
    if (!identity || strcmp(needed, identity))
      return BS_ELF_VERSION_NO_MATCH;
  }
  BsElfVersionExport selected;
  const int status = bs_elf_version_find_export(&view, provider->name,
                                                 symbol, requirement, &selected);
  if (status == BS_ELF_VERSION_MATCH) {
    if (!selected.absolute && selected.symbol_value >= provider->load_size)
      return BS_ELF_VERSION_INVALID;
    *address = selected.absolute ? (uintptr_t)selected.symbol_value :
               (uintptr_t)provider->load_virtbase + selected.symbol_value;
  }
  return status;
}

/* The ABI order is DT_NEEDED (including the importer-specified SONAME) and
 * then the global group for unversioned imports only. A DT_VERNEED import is
 * limited to its named direct dependency and exact VERDEF, or a platform
 * shim explicitly approved for that Bionic ABI.
 * -1 = corrupt/unsupported metadata, 0 = unresolved, 1 = compatible match.
 */
static int so_resolve_symbol(so_module *mod, const BsElfVersionView *importer,
                             DynLibFunction *funcs, int num_funcs,
                             const char *name,
                             const BsElfVersionRequirement *required,
                             uintptr_t *address) {
  *address = 0;
  const bool versioned = required && required->explicit_version;
  if (!versioned) {
    /* Eventfd/epoll own high-range virtual FDs. Intercept only the few I/O
     * calls that must recognize them, delegating every ordinary descriptor
     * to the original GameActivity pipe/fsdev implementation. */
    *address = bs_bionic_event_dispatch_symbol(name);
    if (*address) return BS_ELF_VERSION_MATCH;
    for (int k = 0; k < num_funcs; ++k) {
      if (strcmp(name, funcs[k].symbol) == 0) {
        *address = funcs[k].func;
        return *address ? BS_ELF_VERSION_MATCH : BS_ELF_VERSION_NO_MATCH;
      }
    }
    *address = bs_bionic_extra_symbol(name);
    if (*address) return BS_ELF_VERSION_MATCH;
  } else if (!required->needed_soname || !*required->needed_soname ||
             !required->version_name || !*required->version_name) {
    return BS_ELF_VERSION_INVALID;
  } else {
    /* The Quest APK links against Android system DSOs that aren't staged
     * as guest ELF modules: libc.so, libdl.so and libm.so. Most imports are
     * explicitly versioned @LIBC (including libmain's 12 startup symbols).
     * A real matching Bionic compatibility function is a valid provider
     * when its original DT_NEEDED and version/hash are verified.
     * DO NOT extend this to OpenSSL, unknown Android version namespaces or
     * other providers: they require exact guest ELF VERDEF matching.
     */
    const int shim_allowed =
        bs_elf_version_allows_platform_shim(importer, required);
    if (shim_allowed == BS_ELF_VERSION_INVALID)
      return BS_ELF_VERSION_INVALID;
    if (shim_allowed == BS_ELF_VERSION_MATCH) {
      *address = bs_bionic_event_dispatch_symbol(name);
      if (*address) return BS_ELF_VERSION_MATCH;
      for (int k = 0; k < num_funcs; ++k) {
        if (!strcmp(name, funcs[k].symbol)) {
          *address = funcs[k].func;
          return *address ? BS_ELF_VERSION_MATCH : BS_ELF_VERSION_NO_MATCH;
        }
      }
      *address = bs_bionic_extra_symbol(name);
      if (*address) return BS_ELF_VERSION_MATCH;
    }
  }

  bool required_dependency_found = false;
  for (size_t n = 0; n < importer->needed_count; ++n) {
    const char *needed;
    if (bs_elf_version_dependency(importer, n, &needed) != BS_ELF_VERSION_MATCH)
      return BS_ELF_VERSION_INVALID;
    if (versioned) {
      if (strcmp(needed, required->needed_soname) != 0) continue;
      required_dependency_found = true;
    }
    for (so_module *provider = so_list; provider; provider = provider->next) {
      if (provider == mod) continue;
      int status = so_versioned_provider(provider, needed, name,
                                          versioned ? required : NULL, address);
      if (status == BS_ELF_VERSION_INVALID) return status;
      if (status == BS_ELF_VERSION_MATCH) return status;
    }
  }
  if (versioned) {
    if (!required_dependency_found)
      debugPrintf("[versions] %s: %s requires %s but it is not in DT_NEEDED\n",
                  mod->name, name, required->needed_soname);
    return BS_ELF_VERSION_NO_MATCH;
  }

  /* Unity native-plugin/global fallback is only for UNVERSIONED symbols.
   * Even here, GNU hidden versions and local/TLS/IFUNC exports are excluded.
   */
  for (so_module *provider = so_list; provider; provider = provider->next) {
    if (provider == mod) continue;
    int status = so_versioned_provider(provider, NULL, name, NULL, address);
    if (status != BS_ELF_VERSION_NO_MATCH) return status;
  }
  return BS_ELF_VERSION_NO_MATCH;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  int missing = 0;
  BsElfVersionView importing;
  if (!mod->so_base ||
      bs_elf_version_view_init_rebased(&importing, mod->so_base,
                                       mod->so_size,
                                       (uintptr_t)mod->load_virtbase) !=
      BS_ELF_VERSION_MATCH) {
    debugPrintf("[versions] invalid/unsupported GNU ELF metadata: %s\n", mod->name);
    return -1;
  }
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    size_t count;
    const Elf64_Rela *rels = relocation_entries(mod, i, &count);
    if (rels) {
      for (size_t j = 0; j < count; j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
          {
            if (sym->st_shndx == SHN_UNDEF) {
              char *name = mod->dynstrtab + sym->st_name;
              uintptr_t addr = 0;
              size_t symbol_index = ELF64_R_SYM(rels[j].r_info);
              BsElfVersionRequirement required;
              int need = symbol_index ?
                  bs_elf_version_import(&importing, symbol_index, &required) :
                  BS_ELF_VERSION_NO_MATCH;
              if (need == BS_ELF_VERSION_INVALID) {
                debugPrintf("[versions] %s: invalid version requirement for %s\n",
                            mod->name, name);
                return -1;
              }
              int found = so_resolve_symbol(mod, &importing, funcs, num_funcs,
                    name, need == BS_ELF_VERSION_MATCH ? &required : NULL, &addr);
              if (found == BS_ELF_VERSION_INVALID) {
                debugPrintf("[versions] %s: invalid dependency/export for %s\n",
                            mod->name, name);
                return -1;
              }
              if (addr) {
                *ptr = addr + rels[j].r_addend;
              } else if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                // These supported RELA types use S + A with a zero weak symbol.
                *ptr = rels[j].r_addend;
              } else {
                missing++;
                if (missing <= 32)
                  debugPrintf("%s: unresolved import: %s\n", mod->name, name);
                // Poison unresolved imports when requested.
                if (taint_missing_imports)
                  *ptr = rels[j].r_offset;
              }
            }

            break;
          }

          case R_AARCH64_TLSDESC:
            /* Resolved and validated in so_relocate, including the 16-byte
             * descriptor target and its module-local PT_TLS bounds. */
            break;

          default:
            break;
        }
      }
    }
  }

  if (missing)
    debugPrintf("%s: %d unresolved imports (%d individually logged)\n",
                mod->name, missing, missing < 32 ? missing : 32);

  /* The upstream helper used to report success even with missing imports.
   * The original Unity engine must never execute while platform symbols are
   * unresolved: give the caller the actual unresolved relocation count.
   */
  return missing;
}

void so_execute_init_array(so_module *mod, bool (*should_stop)(void)) {
  debugPrintf("[init] >>> enter init_array for %s\n", mod->name);
  if (mod->init_array_count) {
      int (** init_array)() = (void *)((uintptr_t)mod->load_virtbase + mod->init_array_vaddr);
      int n = (int)mod->init_array_count;
      debugPrintf("[init] %s: %d ctors\n", mod->name, n);
      for (int j = 0; j < n; j++) {
        if (should_stop()) return;
        if (init_array[j] != 0) {
          if (j < 8 || (j % 25) == 0 || j == n - 1)
            debugPrintf("[init] %s %d/%d off=0x%lx\n", mod->name, j, n,
                        (unsigned long)((uintptr_t)init_array[j] - (uintptr_t)mod->load_virtbase));
          init_array[j]();
        }
      }
      debugPrintf("[init] %s: ctors done\n", mod->name);
  }
  debugPrintf("[init] <<< done init_array for %s\n", mod->name);
}

uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  // The source mapping becomes inaccessible after so_finalize. Public
  // function lookups must always return the executable guest mapping.
  return so_find_addr_rx(mod, symbol);
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return addr;
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (mod->load_base == NULL)
    return -1;

  if (mod->so_base) {
    // someone forgot to free the temp data
    so_free_temp(mod);
  }

  Handle process = code_process_handle();
  // remap everything as RW
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    const u64 seg_start = ((u64)mod->load_virtbase + p->p_vaddr) & ~0xFFFull;
    const u64 seg_end = ALIGN_MEM((u64)mod->load_virtbase + p->p_vaddr + p->p_memsz, 0x1000);
    svcSetProcessMemoryPermission(process, seg_start, seg_end - seg_start, Perm_Rw);
  }
  // unmap everything
  svcUnmapProcessCodeMemory(process, (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);

  // release virtual address range
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();

  // remove from list
  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next) {
      if (m->next == mod) {
        m->next = mod->next;
        break;
      }
    }
  }

  return 0;
}

// matches the layout bionic/libunwind expect
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr; // link-time vaddrs + dlpi_addr = runtime
    info.dlpi_phnum = mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}

/* ------------------------------------------------------------------------- */
/* Neutralize bionic TLS stack-canary checks in a loaded module.
 *
 * The engine reads its stack guard from tpidr_el0+0x28, but it installs its own
 * bionic TLS partway through init, so a function that runs across that swap sees
 * a different guard at exit than it saved at entry and calls __stack_chk_fail
 * (a false positive -- the stack isn't actually corrupted). We can't keep the
 * guard perfectly consistent through the engine's own TLS management, so instead
 * we patch the check: find the canary epilogue and NOP the conditional branch to
 * the fail block, so it always falls through to the normal return.
 *
 * Pattern (AArch64):
 *    ldr  Xt, [Xn, #0x28]      ; live guard  (imm12 == 5)
 *    ldr  Xs, [sp, #imm]       ; saved canary
 *    ... (0-4 insns) ...
 *    b.ne <fail>               ; <- NOP this
 *  <fail> contains a bl __stack_chk_fail within a few insns (confirmation).
 *
 * Must run while the code is still writable (load_base), before so_finalize.
 */
void so_patch_stack_canaries(so_module *mod) {
  for (int s = 0; s < mod->elf_hdr->e_shnum; s++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[s].sh_name;
    if (strcmp(sh_name, ".text") != 0)
      continue;
    uintptr_t text_base = (uintptr_t)mod->load_base + mod->sec_hdr[s].sh_addr;
    uint32_t *code = (uint32_t *)text_base;
    int n = (int)(mod->sec_hdr[s].sh_size / 4);
    int patched = 0;
    long first_vaddr = -1, first_word = 0;
    for (int i = 0; i + 12 < n; i++) {
      uint32_t w = code[i];
      /* guard load: ldr Xt,[Xn,#0x28]  (64-bit unsigned-offset, imm12==5) */
      if ((w & 0xFFC00000) != 0xF9400000 || ((w >> 10) & 0xFFF) != 5)
        continue;
      /* saved-canary load from sp within next 2 insns: ldr Xs,[sp,#imm] (Rn==31) */
      int has_sp = 0;
      for (int k = 1; k <= 2; k++) {
        uint32_t w2 = code[i + k];
        if ((w2 & 0xFFC00000) == 0xF9400000 && ((w2 >> 5) & 0x1F) == 31)
          has_sp = 1;
      }
      if (!has_sp)
        continue;
      /* first b.ne within next 8 insns */
      for (int k = 2; k <= 8 && i + k < n; k++) {
        uint32_t w3 = code[i + k];
        if ((w3 & 0xFF000010) == 0x54000000 && (w3 & 0xF) == 1) { /* b.ne */
          int32_t off = (int32_t)((w3 >> 5) & 0x7FFFF);
          if (off & 0x40000) off -= 0x80000;            /* sign-extend imm19 */
          int tgt = i + k + off;
          int has_bl = 0;
          for (int m = 0; m <= 8 && tgt + m < n && tgt >= 0; m++)
            if ((code[tgt + m] & 0xFC000000) == 0x94000000) { has_bl = 1; break; }
          if (has_bl) {
            code[i + k] = 0xD503201F; /* NOP */
            if (first_vaddr < 0) { first_vaddr = (long)(mod->sec_hdr[s].sh_addr + (i + k) * 4); }
            patched++;
          }
          break;
        }
      }
    }
    /* Flush the patched region: we wrote through load_base, but the CPU will fetch
     * from load_virtbase after so_finalize aliases them. Clean dcache for what we
     * wrote so it reaches physical memory before the icache is invalidated. */
    armDCacheFlush((void *)text_base, mod->sec_hdr[s].sh_size);
    if (first_vaddr >= 0)
      first_word = (long)((uint32_t *)((uintptr_t)mod->load_base + first_vaddr))[0];
    debugPrintf("[patch] %s: neutralized %d stack-canary checks (first@0x%lx readback=0x%08lx)\n",
                mod->name, patched, (unsigned long)first_vaddr, (unsigned long)first_word);
  }
}

// ---------------------------------------------------------------------------
// Runtime code patcher. Module text is mapped RX at load_virtbase (via
// svcMapProcessCodeMemory) and is not writable. Create a temporary writable
// alias of the containing pages with svcMapProcessMemory, write through it,
// unmap, and flush caches. Returns 0 on success. Used to hook/replace specific
// il2cpp methods that gate boot (no source recompilation available).
int so_patch_code(void *dst, const void *src, size_t len) {
  uintptr_t start = (uintptr_t)dst & ~0xFFFull;
  size_t maplen = ((((uintptr_t)dst + len) - start) + 0xFFF) & ~0xFFFull;
  size_t off = (uintptr_t)dst - start;
  virtmemLock();
  void *alias = virtmemFindAslr(maplen, 0);
  VirtmemReservation *rv = alias ? virtmemAddReservation(alias, maplen) : NULL;
  virtmemUnlock();
  if (!alias) { debugPrintf("[patch] no aslr slot\n"); return -1; }
  Result rc = svcMapProcessMemory(alias, envGetOwnProcessHandle(), (u64)start, maplen);
  if (R_FAILED(rc)) {
    virtmemLock(); if (rv) virtmemRemoveReservation(rv); virtmemUnlock();
    debugPrintf("[patch] svcMapProcessMemory %p<-0x%lx (%zu) failed rc=%08x\n",
                alias, (unsigned long)start, maplen, rc);
    return -2;
  }
  memcpy((uint8_t *)alias + off, src, len);
  svcUnmapProcessMemory(alias, envGetOwnProcessHandle(), (u64)start, maplen);
  virtmemLock(); if (rv) virtmemRemoveReservation(rv); virtmemUnlock();
  armDCacheFlush(dst, len);
  armICacheInvalidate(dst, len);
  debugPrintf("[patch] ok %p (%zu bytes)\n", dst, len);
  return 0;
}
