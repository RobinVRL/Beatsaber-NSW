/*
 * GNU ELF64 symbol-version lookup for original Android AArch64 .so objects.
 * Portable, read-only file-image parser; no libnx, allocations or game calls.
 * Copyright (c) 2026 Beat Saber NX contributors. MIT license.
 */
#ifndef BS_ELF_SYMBOL_VERSIONS_H
#define BS_ELF_SYMBOL_VERSIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BS_ELF_VERSION_INVALID = -1,
    BS_ELF_VERSION_NO_MATCH = 0,
    BS_ELF_VERSION_MATCH = 1
};

/*
 * Every string and image pointer in these structures borrows storage from
 * image. The owner must keep the original ELF file image alive throughout
 * resolution (before so_free_temp), and must not mutate it during a lookup.
 */
typedef struct BsElfVersionView {
    const uint8_t *image;
    size_t image_size;
    uint64_t phoff;
    uint16_t phnum;
    uint64_t program_header_bias;
    uint64_t dynamic_file_offset;
    size_t dynamic_file_size;
    size_t needed_count;
    uint64_t symtab_vaddr;
    uint64_t strtab_vaddr;
    uint64_t versym_vaddr;
    uint64_t verneed_vaddr;
    uint64_t verdef_vaddr;
    uint64_t soname_offset;
    size_t symbol_count;
    size_t strtab_size;
    uint16_t verneed_count;
    uint16_t verdef_count;
    bool has_versym;
    bool has_verneed;
    bool has_verdef;
    bool has_soname;
    const char *soname;
} BsElfVersionView;

typedef struct BsElfVersionRequirement {
    const char *symbol_name;
    const char *version_name;  /* NULL for an unversioned import */
    const char *needed_soname; /* DT_VERNEED vn_file, if versioned */
    uint32_t version_hash;     /* DT_VERNEED vna_hash */
    uint16_t version_index;    /* DT_VERSYM & 0x7fff */
    bool explicit_version;
} BsElfVersionRequirement;

typedef struct BsElfVersionExport {
    size_t symbol_index;
    uint64_t symbol_value;  /* ELF st_value; caller applies load bias */
    uint64_t symbol_size;
    uint8_t symbol_type;    /* STT_FUNC, STT_OBJECT or STT_NOTYPE */
    uint8_t symbol_binding;
    uint8_t visibility;
    uint16_t section_index;
    bool absolute;          /* SHN_ABS: do not add load bias */
} BsElfVersionExport;

/*
 * Validates little-endian ET_DYN/AArch64, PT_LOAD, PT_DYNAMIC, dynsym and
 * strtab boundaries, DT_VERSYM, DT_VERNEED and DT_VERDEF chains, strings,
 * and symbol counts. Works even if section headers have been stripped when
 * DT_HASH or DT_GNU_HASH supplies a validated dynamic symbol count.
 */
int bs_elf_version_view_init(BsElfVersionView *out,
                             const void *file_image, size_t file_size);

/*
 * so_load() modifies every ELF program header's p_vaddr in its temporary
 * file image by adding load_virtbase. For those already-staged images pass
 * (uintptr_t)mod->load_virtbase as program_header_bias. The immutable
 * PT_DYNAMIC tags and section addresses still contain link-time vaddrs.
 * A pristine on-disk ELF uses bs_elf_version_view_init() (zero bias).
 */
int bs_elf_version_view_init_rebased(BsElfVersionView *out,
                                     const void *file_image, size_t file_size,
                                     uint64_t program_header_bias);

/* Read version requirements for a relocation's ELF64_R_SYM index. */
int bs_elf_version_import(const BsElfVersionView *importer,
                          size_t symbol_index,
                          BsElfVersionRequirement *out);

/* Enumerate validated DT_NEEDED names in their original ELF order.
 * Returns 1 for an entry, 0 after the last entry, -1 if metadata is invalid.
 * Strings borrow the source image and must not outlive it.
 */
int bs_elf_version_dependency(const BsElfVersionView *importer,
                              size_t dependency_index, const char **name);

/* A native platform shim may replace a Bionic system-library import only
 * for the original base LIBC version, with valid ELF hash and an actual
 * DT_NEEDED dependency on libc.so, libdl.so or libm.so. Unknown GNU versions,
 * OpenSSL versions and other ELF providers MUST NOT bypass VERDEF matching.
 * 1 = eligible for exact-name shim lookup, 0 = not eligible, -1 = corrupt.
 */
int bs_elf_version_allows_platform_shim(
    const BsElfVersionView *importer,
    const BsElfVersionRequirement *requirement);

/*
 * Resolve a named export in exactly one provider. If requirement is NULL,
 * behaves like an unversioned dlsym (never selects a hidden version).
 * provider_filename may be the basename or full path. A versioned import
 * requires the provider's SONAME or filename to match DT_VERNEED vn_file.
 *
 * Return 1 only for a validated compatible non-TLS, non-IFUNC export;
 * 0 for no compatible symbol; -1 for corrupt/unsupported ELF metadata.
 * No function is executed and no address is dereferenced by this helper.
 */
int bs_elf_version_find_export(const BsElfVersionView *provider,
                               const char *provider_filename,
                               const char *symbol_name,
                               const BsElfVersionRequirement *requirement,
                               BsElfVersionExport *out);

#endif
