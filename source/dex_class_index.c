/* Read-only class_def index of the ORIGINAL Quest classes.dex.
 * See dex_class_index.h. */
#include "dex_class_index.h"

#include <stdlib.h>
#include <string.h>

typedef struct BsDexClassEntry {
    char *name;       /* internal name, no L...; */
    char *superclass; /* internal name or "" */
} BsDexClassEntry;

static BsDexClassEntry *g_entries;
static size_t g_entry_count;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Return a NUL-terminated MUTF-8 descriptor for type `type_idx`. */
static const char *dex_type_descriptor(const uint8_t *dex, size_t size,
                                       uint32_t type_idx) {
    const uint32_t string_ids_size = rd32(dex + 0x38);
    const uint32_t string_ids_off = rd32(dex + 0x3c);
    const uint32_t type_ids_size = rd32(dex + 0x40);
    const uint32_t type_ids_off = rd32(dex + 0x44);
    if (type_idx >= type_ids_size ||
        (uint64_t)type_ids_off + 4ull * type_ids_size > size) return NULL;
    const uint32_t string_idx = rd32(dex + type_ids_off + 4ull * type_idx);
    if (string_idx >= string_ids_size ||
        (uint64_t)string_ids_off + 4ull * string_ids_size > size) return NULL;
    size_t off = rd32(dex + string_ids_off + 4ull * string_idx);
    /* Skip the uleb128 utf16_size prefix. */
    for (int i = 0; i < 5; ++i) {
        if (off >= size) return NULL;
        if (!(dex[off++] & 0x80)) break;
        if (i == 4) return NULL;
    }
    const uint8_t *end = memchr(dex + off, 0, size - off);
    return end ? (const char *)(dex + off) : NULL;
}

/* "Lfoo/Bar;" -> malloc'd "foo/Bar". Array/primitive descriptors rejected. */
static char *internal_from_descriptor(const char *descriptor) {
    if (!descriptor || descriptor[0] != 'L') return NULL;
    const size_t length = strlen(descriptor);
    if (length < 3 || descriptor[length - 1] != ';') return NULL;
    char *name = malloc(length - 1);
    if (!name) return NULL;
    memcpy(name, descriptor + 1, length - 2);
    name[length - 2] = '\0';
    return name;
}

void bs_dex_class_index_reset(void) {
    for (size_t i = 0; i < g_entry_count; ++i) {
        free(g_entries[i].name);
        free(g_entries[i].superclass);
    }
    free(g_entries);
    g_entries = NULL;
    g_entry_count = 0;
}

size_t bs_dex_class_index_build(const uint8_t *dex, size_t size) {
    bs_dex_class_index_reset();
    if (!dex || size < 0x70 || memcmp(dex, "dex\n", 4)) return 0;
    const uint32_t class_defs_size = rd32(dex + 0x60);
    const uint32_t class_defs_off = rd32(dex + 0x64);
    if (!class_defs_size ||
        (uint64_t)class_defs_off + 32ull * class_defs_size > size) return 0;
    BsDexClassEntry *entries = calloc(class_defs_size, sizeof *entries);
    if (!entries) return 0;
    size_t count = 0;
    for (uint32_t i = 0; i < class_defs_size; ++i) {
        const uint8_t *def = dex + class_defs_off + 32ull * i;
        char *name = internal_from_descriptor(
            dex_type_descriptor(dex, size, rd32(def)));
        const uint32_t super_idx = rd32(def + 8);
        char *superclass = super_idx == UINT32_MAX ? calloc(1, 1) :
            internal_from_descriptor(dex_type_descriptor(dex, size, super_idx));
        if (!name || !superclass) {
            free(name);
            free(superclass);
            for (size_t k = 0; k < count; ++k) {
                free(entries[k].name);
                free(entries[k].superclass);
            }
            free(entries);
            return 0;
        }
        entries[count].name = name;
        entries[count].superclass = superclass;
        ++count;
    }
    g_entries = entries;
    g_entry_count = count;
    return count;
}

size_t bs_dex_class_index_count(void) {
    return g_entry_count;
}

int bs_dex_class_index_lookup(const char *internal_name,
                              char *super_out, size_t super_cap) {
    if (!internal_name) return 0;
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (strcmp(g_entries[i].name, internal_name)) continue;
        if (super_out && super_cap) {
            const size_t length = strlen(g_entries[i].superclass);
            if (length >= super_cap) return 0;
            memcpy(super_out, g_entries[i].superclass, length + 1);
        }
        return 1;
    }
    return 0;
}
