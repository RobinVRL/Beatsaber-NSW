/* Portable GNU ELF64 versioned symbol resolution. See elf_symbol_versions.h. */
#include "elf_symbol_versions.h"

#include <limits.h>
#include <string.h>

enum {
    BS_PT_LOAD = 1, BS_PT_DYNAMIC = 2,
    BS_SHT_DYNSYM = 11,
    BS_DT_NULL = 0, BS_DT_HASH = 4, BS_DT_STRTAB = 5,
    BS_DT_SYMTAB = 6, BS_DT_STRSZ = 10, BS_DT_SYMENT = 11,
    BS_DT_SONAME = 14, BS_DT_NEEDED = 1,
    BS_DT_GNU_HASH = 0x6ffffef5,
    BS_DT_VERSYM = 0x6ffffff0,
    BS_DT_VERDEF = 0x6ffffffc, BS_DT_VERDEFNUM = 0x6ffffffd,
    BS_DT_VERNEED = 0x6ffffffe, BS_DT_VERNEEDNUM = 0x6fffffff,
    BS_STB_GLOBAL = 1, BS_STB_WEAK = 2, BS_STB_GNU_UNIQUE = 10,
    BS_STT_TLS = 6, BS_STT_GNU_IFUNC = 10,
    BS_STV_DEFAULT = 0, BS_STV_PROTECTED = 3,
    BS_SHN_UNDEF = 0, BS_SHN_ABS = 0xfff1,
    BS_VER_NDX_MASK = 0x7fff, BS_VER_NDX_HIDDEN = 0x8000,
    BS_VER_FLG_BASE = 1,
    BS_MAX_SYMBOLS = 1000000, BS_MAX_VERSION_RECORDS = 4096
};

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static bool in_range(size_t whole, uint64_t start, uint64_t length) {
    return start <= whole && length <= (uint64_t)whole - start;
}

static bool add64(uint64_t a, uint64_t b, uint64_t *sum) {
    if (b > UINT64_MAX - a) return false;
    *sum = a + b;
    return true;
}

static const uint8_t *program_header(const BsElfVersionView *v, size_t index) {
    return v->image + (size_t)v->phoff + index * 56;
}

static bool link_time_address(const BsElfVersionView *v, const uint8_t *p,
                              uint64_t *address) {
    uint64_t addr = rd64(p + 16);
    if (addr < v->program_header_bias) return false;
    *address = addr - v->program_header_bias;
    return true;
}

/* ELF virtual address -> original file bytes. Never use PT_LOAD memsz here:
 * BSS is not backed by the file and cannot hold dynamic ELF metadata. */
static const uint8_t *image_at(const BsElfVersionView *v, uint64_t va,
                               uint64_t length) {
    for (size_t i = 0; i < v->phnum; ++i) {
        const uint8_t *p = program_header(v, i);
        if (rd32(p) != BS_PT_LOAD) continue;
        uint64_t start;
        if (!link_time_address(v, p, &start)) return NULL;
        uint64_t file_size = rd64(p + 32);
        if (va < start || va - start > file_size ||
            length > file_size - (va - start)) continue;
        uint64_t off;
        if (!add64(rd64(p + 8), va - start, &off) ||
            !in_range(v->image_size, off, length)) return NULL;
        return v->image + (size_t)off;
    }
    return NULL;
}

static bool symbol_address_valid(const BsElfVersionView *v,
                                 uint64_t address, uint64_t size) {
    for (size_t i = 0; i < v->phnum; ++i) {
        const uint8_t *p = program_header(v, i);
        if (rd32(p) != BS_PT_LOAD) continue;
        uint64_t start;
        if (!link_time_address(v, p, &start)) return false;
        const uint64_t len = rd64(p + 40);
        if (address >= start && address - start < len &&
            size <= len - (address - start)) return true;
    }
    return false;
}

static const char *dynamic_string(const BsElfVersionView *v, uint64_t offset) {
    if (offset >= v->strtab_size) return NULL;
    const uint8_t *base = image_at(v, v->strtab_vaddr, v->strtab_size);
    if (!base) return NULL;
    const char *p = (const char *)base + (size_t)offset;
    return memchr(p, 0, v->strtab_size - (size_t)offset) ? p : NULL;
}

static bool symbol_at(const BsElfVersionView *v, size_t index,
                      const uint8_t **symbol) {
    if (index >= v->symbol_count || index > UINT64_MAX / 24) return false;
    uint64_t va;
    if (!add64(v->symtab_vaddr, index * 24, &va)) return false;
    *symbol = image_at(v, va, 24);
    return *symbol != NULL;
}

static int version_index(const BsElfVersionView *v, size_t index,
                         uint16_t *value) {
    if (!v->has_versym) { *value = 1; return BS_ELF_VERSION_MATCH; }
    if (index >= v->symbol_count || index > UINT64_MAX / 2) return BS_ELF_VERSION_INVALID;
    uint64_t va;
    if (!add64(v->versym_vaddr, index * 2, &va)) return BS_ELF_VERSION_INVALID;
    const uint8_t *p = image_at(v, va, 2);
    if (!p) return BS_ELF_VERSION_INVALID;
    *value = rd16(p);
    return BS_ELF_VERSION_MATCH;
}

/*
 * Validate each DT_VERNEED chain entry and return the requested version by
 * DT_VERSYM's 15-bit index. A repeated index is ambiguous and rejected.
 */
static int find_needed_version(const BsElfVersionView *v, uint16_t wanted,
                               const char **name, const char **library,
                               uint32_t *hash) {
    uint64_t cursor = v->verneed_vaddr;
    int found = 0;
    for (size_t i = 0; i < v->verneed_count; ++i) {
        const uint8_t *vn = image_at(v, cursor, 16);
        if (!vn || rd16(vn) != 1 || !rd16(vn + 2) ||
            rd16(vn + 2) > BS_MAX_VERSION_RECORDS || rd32(vn + 8) < 16)
            return BS_ELF_VERSION_INVALID;
        const char *file = dynamic_string(v, rd32(vn + 4));
        if (!file || !*file) return BS_ELF_VERSION_INVALID;
        uint64_t aux;
        if (!add64(cursor, rd32(vn + 8), &aux)) return BS_ELF_VERSION_INVALID;
        for (size_t j = 0; j < rd16(vn + 2); ++j) {
            const uint8_t *a = image_at(v, aux, 16);
            if (!a) return BS_ELF_VERSION_INVALID;
            const char *version = dynamic_string(v, rd32(a + 8));
            if (!version || !*version) return BS_ELF_VERSION_INVALID;
            if (wanted && (rd16(a + 6) & BS_VER_NDX_MASK) == wanted) {
                if (++found > 1) return BS_ELF_VERSION_INVALID;
                if (name) *name = version;
                if (library) *library = file;
                if (hash) *hash = rd32(a);
            }
            if (j + 1 < rd16(vn + 2)) {
                if (rd32(a + 12) < 16 || !add64(aux, rd32(a + 12), &aux))
                    return BS_ELF_VERSION_INVALID;
            }
        }
        if (i + 1 < v->verneed_count) {
            if (rd32(vn + 12) < 16 || !add64(cursor, rd32(vn + 12), &cursor))
                return BS_ELF_VERSION_INVALID;
        }
    }
    return found ? BS_ELF_VERSION_MATCH : BS_ELF_VERSION_NO_MATCH;
}

static int find_defined_version(const BsElfVersionView *v, uint16_t wanted,
                                const char **name, uint32_t *hash,
                                uint16_t *flags) {
    uint64_t cursor = v->verdef_vaddr;
    int found = 0;
    for (size_t i = 0; i < v->verdef_count; ++i) {
        const uint8_t *vd = image_at(v, cursor, 20);
        if (!vd || rd16(vd) != 1 || !rd16(vd + 6) ||
            rd16(vd + 6) > BS_MAX_VERSION_RECORDS || rd32(vd + 12) < 20)
            return BS_ELF_VERSION_INVALID;
        uint64_t aux;
        if (!add64(cursor, rd32(vd + 12), &aux)) return BS_ELF_VERSION_INVALID;
        const char *first = NULL;
        for (size_t j = 0; j < rd16(vd + 6); ++j) {
            const uint8_t *a = image_at(v, aux, 8);
            if (!a) return BS_ELF_VERSION_INVALID;
            const char *n = dynamic_string(v, rd32(a));
            if (!n || !*n) return BS_ELF_VERSION_INVALID;
            if (j == 0) first = n;
            if (j + 1 < rd16(vd + 6)) {
                if (rd32(a + 4) < 8 || !add64(aux, rd32(a + 4), &aux))
                    return BS_ELF_VERSION_INVALID;
            }
        }
        if (wanted && (rd16(vd + 4) & BS_VER_NDX_MASK) == wanted) {
            if (++found > 1) return BS_ELF_VERSION_INVALID;
            if (name) *name = first;
            if (hash) *hash = rd32(vd + 8);
            if (flags) *flags = rd16(vd + 2);
        }
        if (i + 1 < v->verdef_count) {
            if (rd32(vd + 16) < 20 || !add64(cursor, rd32(vd + 16), &cursor))
                return BS_ELF_VERSION_INVALID;
        }
    }
    return found ? BS_ELF_VERSION_MATCH : BS_ELF_VERSION_NO_MATCH;
}

/* Determine a symbol count using the original SHT_DYNSYM if available, then
 * DT_HASH, then DT_GNU_HASH. GNU hash buckets/chains are bounded by PT_LOAD. */
static bool symbol_count(const BsElfVersionView *v, uint64_t hash,
                         uint64_t gnu_hash, size_t *out) {
    if (v->image_size >= 64 && rd64(v->image + 40) &&
        rd16(v->image + 60) && rd16(v->image + 58) == 64) {
        uint64_t shoff = rd64(v->image + 40);
        uint16_t shnum = rd16(v->image + 60);
        if (!in_range(v->image_size, shoff, (uint64_t)shnum * 64)) return false;
        for (size_t i = 0; i < shnum; ++i) {
            const uint8_t *s = v->image + (size_t)shoff + 64 * i;
            if (rd32(s + 4) != BS_SHT_DYNSYM ||
                rd64(s + 16) != v->symtab_vaddr) continue;
            if (rd64(s + 56) != 24 || rd64(s + 32) % 24 ||
                rd64(s + 32) / 24 > BS_MAX_SYMBOLS) return false;
            *out = (size_t)(rd64(s + 32) / 24);
            return *out > 0;
        }
    }
    if (hash) {
        const uint8_t *p = image_at(v, hash, 8);
        if (!p || !rd32(p + 4) || rd32(p + 4) > BS_MAX_SYMBOLS ||
            rd32(p) > BS_MAX_SYMBOLS) return false;
        uint64_t table = 8u + ((uint64_t)rd32(p) + rd32(p + 4)) * 4;
        if (!image_at(v, hash, table)) return false;
        *out = rd32(p + 4);
        return true;
    }
    if (!gnu_hash) return false;
    const uint8_t *h = image_at(v, gnu_hash, 16);
    if (!h || !rd32(h) || rd32(h) > BS_MAX_SYMBOLS ||
        !rd32(h + 8) || rd32(h + 8) > BS_MAX_SYMBOLS ||
        rd32(h + 4) > BS_MAX_SYMBOLS) return false;
    uint64_t buckets_va, chain_va;
    if (!add64(gnu_hash, 16u + (uint64_t)rd32(h + 8) * 8, &buckets_va) ||
        !add64(buckets_va, (uint64_t)rd32(h) * 4, &chain_va) ||
        !image_at(v, buckets_va, (uint64_t)rd32(h) * 4)) return false;
    size_t maximum = rd32(h + 4);
    for (uint32_t i = 0; i < rd32(h); ++i) {
        const uint8_t *b = image_at(v, buckets_va + (uint64_t)i * 4, 4);
        if (!b) return false;
        uint32_t idx = rd32(b);
        if (!idx) continue;
        if (idx < rd32(h + 4)) return false;
        for (;;) {
            if (idx >= BS_MAX_SYMBOLS) return false;
            uint64_t va;
            if (!add64(chain_va, ((uint64_t)idx - rd32(h + 4)) * 4, &va))
                return false;
            const uint8_t *chain = image_at(v, va, 4);
            if (!chain) return false;
            if ((size_t)idx + 1 > maximum) maximum = (size_t)idx + 1;
            if (rd32(chain) & 1) break;
            ++idx;
        }
    }
    *out = maximum;
    return maximum > 0;
}

int bs_elf_version_view_init_rebased(BsElfVersionView *out,
                                     const void *file_image, size_t file_size,
                                     uint64_t program_header_bias) {
    if (!out) return BS_ELF_VERSION_INVALID;
    memset(out, 0, sizeof(*out));
    if (!file_image || file_size < 64) return BS_ELF_VERSION_INVALID;
    BsElfVersionView v = {0};
    v.image = (const uint8_t *)file_image;
    v.image_size = file_size;
    v.program_header_bias = program_header_bias;
    const uint8_t *eh = v.image;
    if (memcmp(eh, "\177ELF", 4) || eh[4] != 2 || eh[5] != 1 ||
        rd16(eh + 16) != 3 || rd16(eh + 18) != 183 ||
        rd16(eh + 52) != 64 || rd16(eh + 54) != 56 ||
        !rd16(eh + 56)) return BS_ELF_VERSION_INVALID;
    v.phoff = rd64(eh + 32);
    v.phnum = rd16(eh + 56);
    if (!in_range(file_size, v.phoff, (uint64_t)v.phnum * 56))
        return BS_ELF_VERSION_INVALID;
    /* A declared section table must be well formed. Sectionless libraries
     * are valid and use DT_HASH/DT_GNU_HASH instead. */
    if (rd16(eh + 60) &&
        (rd16(eh + 58) != 64 ||
         !in_range(file_size, rd64(eh + 40), (uint64_t)rd16(eh + 60) * 64)))
        return BS_ELF_VERSION_INVALID;
    uint64_t hash = 0, gnu_hash = 0, sym_entsize = 0, soname = 0;
    uint64_t verneed_num = 0, verdef_num = 0;
    bool has_symtab = false, has_strtab = false, has_strsz = false;
    bool has_soname = false, seen_dynamic = false, has_null = false;
    for (size_t i = 0; i < v.phnum; ++i) {
        const uint8_t *p = program_header(&v, i);
        uint32_t type = rd32(p);
        uint64_t off = rd64(p + 8), filesz = rd64(p + 32);
        if (!in_range(file_size, off, filesz)) return BS_ELF_VERSION_INVALID;
        if (type == BS_PT_LOAD) {
            uint64_t link_address;
            if (!link_time_address(&v, p, &link_address))
                return BS_ELF_VERSION_INVALID;
            if (filesz > rd64(p + 40) ||
                link_address > UINT64_MAX - rd64(p + 40))
                return BS_ELF_VERSION_INVALID;
        }
        if (type != BS_PT_DYNAMIC) continue;
        if (seen_dynamic || filesz % 16) return BS_ELF_VERSION_INVALID;
        seen_dynamic = true;
        v.dynamic_file_offset = off;
        v.dynamic_file_size = (size_t)filesz;
        for (uint64_t j = 0; j < filesz; j += 16) {
            const uint8_t *d = v.image + (size_t)off + (size_t)j;
            uint64_t tag = rd64(d), value = rd64(d + 8);
            if (tag == BS_DT_NULL) { has_null = true; break; }
            switch (tag) {
                case BS_DT_SYMTAB: has_symtab = true; v.symtab_vaddr = value; break;
                case BS_DT_STRTAB: has_strtab = true; v.strtab_vaddr = value; break;
                case BS_DT_STRSZ: has_strsz = true; v.strtab_size = (size_t)value;
                    if ((uint64_t)v.strtab_size != value) return BS_ELF_VERSION_INVALID;
                    break;
                case BS_DT_SYMENT: sym_entsize = value; break;
                case BS_DT_HASH: hash = value; break;
                case BS_DT_GNU_HASH: gnu_hash = value; break;
                case BS_DT_VERSYM: v.has_versym = true; v.versym_vaddr = value; break;
                case BS_DT_VERNEED: v.has_verneed = true; v.verneed_vaddr = value; break;
                case BS_DT_VERNEEDNUM: verneed_num = value; break;
                case BS_DT_VERDEF: v.has_verdef = true; v.verdef_vaddr = value; break;
                case BS_DT_VERDEFNUM: verdef_num = value; break;
                case BS_DT_SONAME: has_soname = true; soname = value; break;
                case BS_DT_NEEDED:
                    if (++v.needed_count > BS_MAX_VERSION_RECORDS)
                        return BS_ELF_VERSION_INVALID;
                    break;
                default: break;
            }
        }
    }
    if (!seen_dynamic || !has_null || !has_symtab || !has_strtab ||
        !has_strsz || !v.strtab_size || sym_entsize != 24 ||
        !image_at(&v, v.strtab_vaddr, v.strtab_size) ||
        (v.has_verneed != (verneed_num != 0)) ||
        (v.has_verdef != (verdef_num != 0)) ||
        verneed_num > BS_MAX_VERSION_RECORDS ||
        verdef_num > BS_MAX_VERSION_RECORDS ||
        !symbol_count(&v, hash, gnu_hash, &v.symbol_count) ||
        !image_at(&v, v.symtab_vaddr, (uint64_t)v.symbol_count * 24))
        return BS_ELF_VERSION_INVALID;
    if (v.has_versym &&
        !image_at(&v, v.versym_vaddr, (uint64_t)v.symbol_count * 2))
        return BS_ELF_VERSION_INVALID;
    v.verneed_count = (uint16_t)verneed_num;
    v.verdef_count = (uint16_t)verdef_num;
    if ((v.has_verneed &&
         find_needed_version(&v, 0, NULL, NULL, NULL) < 0) ||
        (v.has_verdef &&
         find_defined_version(&v, 0, NULL, NULL, NULL) < 0))
        return BS_ELF_VERSION_INVALID;
    if (has_soname) {
        v.soname = dynamic_string(&v, soname);
        if (!v.soname || !*v.soname) return BS_ELF_VERSION_INVALID;
        v.has_soname = true;
        v.soname_offset = soname;
    }
    for (size_t i = 0; i < v.needed_count; ++i) {
        const char *dependency = NULL;
        if (bs_elf_version_dependency(&v, i, &dependency) != BS_ELF_VERSION_MATCH ||
            !dependency || !*dependency)
            return BS_ELF_VERSION_INVALID;
    }
    *out = v;
    return BS_ELF_VERSION_MATCH;
}

int bs_elf_version_view_init(BsElfVersionView *out,
                             const void *file_image, size_t file_size) {
    return bs_elf_version_view_init_rebased(out, file_image, file_size, 0);
}

int bs_elf_version_dependency(const BsElfVersionView *importer,
                              size_t dependency_index, const char **name) {
    if (!importer || !importer->image || !name ||
        !in_range(importer->image_size, importer->dynamic_file_offset,
                  importer->dynamic_file_size) ||
        importer->dynamic_file_size % 16)
        return BS_ELF_VERSION_INVALID;
    *name = NULL;
    if (dependency_index >= importer->needed_count) return BS_ELF_VERSION_NO_MATCH;
    size_t found = 0;
    for (size_t offset = 0; offset < importer->dynamic_file_size; offset += 16) {
        const uint8_t *d = importer->image +
                           (size_t)importer->dynamic_file_offset + offset;
        uint64_t tag = rd64(d);
        if (tag == BS_DT_NULL) break;
        if (tag != BS_DT_NEEDED) continue;
        if (found++ != dependency_index) continue;
        const char *s = dynamic_string(importer, rd64(d + 8));
        if (!s || !*s) return BS_ELF_VERSION_INVALID;
        *name = s;
        return BS_ELF_VERSION_MATCH;
    }
    return BS_ELF_VERSION_INVALID;
}

static uint32_t bionic_elf_hash(const char *name) {
    uint32_t hash = 0;
    for (; *name; ++name) {
        hash = (hash << 4) + (uint8_t)*name;
        const uint32_t high = hash & UINT32_C(0xf0000000);
        if (high) hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

int bs_elf_version_allows_platform_shim(
    const BsElfVersionView *importer,
    const BsElfVersionRequirement *requirement) {
    if (!importer || !importer->image || !requirement)
        return BS_ELF_VERSION_INVALID;
    if (!requirement->explicit_version || !requirement->symbol_name ||
        !requirement->needed_soname || !requirement->version_name)
        return BS_ELF_VERSION_NO_MATCH;
    const char *version = requirement->version_name;
    const char *soname = requirement->needed_soname;
    if (requirement->version_hash != bionic_elf_hash(version))
        return BS_ELF_VERSION_NO_MATCH;
    if (!strcmp(version, "LIBC")) {
        if (strcmp(soname, "libc.so") && strcmp(soname, "libdl.so") &&
            strcmp(soname, "libm.so"))
            return BS_ELF_VERSION_NO_MATCH;
    } else {
        /* AOSP bionic/libc/libc.map.txt lists these Android ARM64 exports.
         * Each has a matching implementation in the platform bridge. */
        static const struct { const char *version, *symbol; } supported[] = {
            {"LIBC_N", "__fwrite_chk"},
            {"LIBC_N", "__pwrite_chk"},
            {"LIBC_N", "__write_chk"},
            {"LIBC_N", "fseeko64"},
            {"LIBC_N", "ftello64"},
            {"LIBC_O", "strtod_l"},
            {"LIBC_O", "strtof_l"},
            {"LIBC_P", "aligned_alloc"},
            {"LIBC_P", "getentropy"},
        };
        if (strcmp(soname, "libc.so")) return BS_ELF_VERSION_NO_MATCH;
        bool allowed = false;
        for (size_t i = 0; i < sizeof(supported)/sizeof(supported[0]); ++i) {
            if (strcmp(version, supported[i].version) ||
                strcmp(requirement->symbol_name, supported[i].symbol))
                continue;
            allowed = true;
            break;
        }
        if (!allowed) return BS_ELF_VERSION_NO_MATCH;
    }
    for (size_t i = 0; i < importer->needed_count; ++i) {
        const char *needed = NULL;
        int status = bs_elf_version_dependency(importer, i, &needed);
        if (status == BS_ELF_VERSION_INVALID) return status;
        if (status == BS_ELF_VERSION_MATCH && !strcmp(needed, soname))
            return BS_ELF_VERSION_MATCH;
    }
    return BS_ELF_VERSION_NO_MATCH;
}

int bs_elf_version_import(const BsElfVersionView *importer,
                          size_t symbol_index,
                          BsElfVersionRequirement *out) {
    if (!importer || !out || !importer->image) return BS_ELF_VERSION_INVALID;
    memset(out, 0, sizeof(*out));
    const uint8_t *sym;
    if (!symbol_at(importer, symbol_index, &sym) ||
        rd16(sym + 6) != BS_SHN_UNDEF) return BS_ELF_VERSION_INVALID;
    const char *name = dynamic_string(importer, rd32(sym));
    if (!name || !*name) return BS_ELF_VERSION_INVALID;
    uint16_t idx;
    if (version_index(importer, symbol_index, &idx) < 0)
        return BS_ELF_VERSION_INVALID;
    out->symbol_name = name;
    out->version_index = idx & BS_VER_NDX_MASK;
    if (out->version_index > 1) {
        if (!importer->has_verneed ||
            find_needed_version(importer, out->version_index,
                                &out->version_name, &out->needed_soname,
                                &out->version_hash) != BS_ELF_VERSION_MATCH)
            return BS_ELF_VERSION_INVALID;
        out->explicit_version = true;
    }
    return BS_ELF_VERSION_MATCH;
}

static const char *basename_only(const char *path) {
    if (!path) return NULL;
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    const char *last = (a && b) ? (a > b ? a : b) : (a ? a : b);
    return last ? last + 1 : path;
}

int bs_elf_version_find_export(const BsElfVersionView *provider,
                               const char *provider_filename,
                               const char *symbol_name,
                               const BsElfVersionRequirement *requirement,
                               BsElfVersionExport *out) {
    if (!provider || !provider->image || !symbol_name || !*symbol_name || !out)
        return BS_ELF_VERSION_INVALID;
    memset(out, 0, sizeof(*out));
    if (requirement && requirement->symbol_name &&
        strcmp(symbol_name, requirement->symbol_name))
        return BS_ELF_VERSION_INVALID;
    bool explicit_version = requirement && requirement->explicit_version;
    if (explicit_version) {
        if (!requirement->version_name || !requirement->needed_soname ||
            !*requirement->version_name || !*requirement->needed_soname)
            return BS_ELF_VERSION_INVALID;
        const char *base = basename_only(provider_filename);
        /* Prefer the ELF's own DT_SONAME; a renamed file does not change the
         * identity specified by its dynamic-linker metadata. */
        const char *identity = provider->soname ? provider->soname : base;
        if (!identity || strcmp(identity, requirement->needed_soname))
            return BS_ELF_VERSION_NO_MATCH;
    }
    for (size_t i = 0; i < provider->symbol_count; ++i) {
        const uint8_t *sym;
        if (!symbol_at(provider, i, &sym)) return BS_ELF_VERSION_INVALID;
        uint8_t binding = sym[4] >> 4, type = sym[4] & 15;
        uint8_t visibility = sym[5] & 3;
        uint16_t section = rd16(sym + 6);
        if (section == BS_SHN_UNDEF ||
            (binding != BS_STB_GLOBAL && binding != BS_STB_WEAK &&
             binding != BS_STB_GNU_UNIQUE) ||
            (visibility != BS_STV_DEFAULT && visibility != BS_STV_PROTECTED))
            continue;
        if (section >= 0xff00 && section != BS_SHN_ABS)
            return BS_ELF_VERSION_INVALID;
        const char *name = dynamic_string(provider, rd32(sym));
        if (!name) return BS_ELF_VERSION_INVALID;
        if (strcmp(name, symbol_name)) continue;
        /* Guest TLS values are offsets, and IFUNCs require executing an ELF
         * resolver. Neither may be returned as an ordinary code/data address. */
        if (type == BS_STT_TLS || type == BS_STT_GNU_IFUNC) continue;
        uint64_t value = rd64(sym + 8), size = rd64(sym + 16);
        if (section != BS_SHN_ABS && !symbol_address_valid(provider, value, size))
            return BS_ELF_VERSION_INVALID;
        uint16_t raw;
        if (version_index(provider, i, &raw) < 0)
            return BS_ELF_VERSION_INVALID;
        uint16_t idx = raw & BS_VER_NDX_MASK;
        if (idx == 0) continue;
        if (explicit_version) {
            if (idx <= 1 || !provider->has_verdef) continue;
            const char *version = NULL;
            uint32_t hash = 0;
            uint16_t flags = 0;
            int status = find_defined_version(provider, idx, &version, &hash, &flags);
            if (status == BS_ELF_VERSION_INVALID) return status;
            if (status == BS_ELF_VERSION_NO_MATCH) return BS_ELF_VERSION_INVALID;
            if ((flags & BS_VER_FLG_BASE) ||
                hash != requirement->version_hash ||
                strcmp(version, requirement->version_name)) continue;
        } else if (raw & BS_VER_NDX_HIDDEN) {
            continue;
        } else if (idx > 1) {
            /* dlsym(name) may select the public default version only. */
            if (raw & BS_VER_NDX_HIDDEN) continue;
            if (!provider->has_verdef) return BS_ELF_VERSION_INVALID;
            uint16_t flags = 0;
            int status = find_defined_version(provider, idx, NULL, NULL, &flags);
            if (status != BS_ELF_VERSION_MATCH) return BS_ELF_VERSION_INVALID;
            if (flags & BS_VER_FLG_BASE) continue;
        }
        out->symbol_index = i;
        out->symbol_value = value;
        out->symbol_size = size;
        out->symbol_type = type;
        out->symbol_binding = binding;
        out->visibility = visibility;
        out->section_index = section;
        out->absolute = section == BS_SHN_ABS;
        return BS_ELF_VERSION_MATCH;
    }
    return BS_ELF_VERSION_NO_MATCH;
}
