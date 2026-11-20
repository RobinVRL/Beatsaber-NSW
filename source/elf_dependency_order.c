/* Original ELF DT_NEEDED -> constructor precedence graph.
 * Portable for isolated host tests, with one small libnx so_module adapter.
 * Copyright (c) 2026 Beat Saber NX contributors. MIT license.
 */
#include "elf_dependency_order.h"

#include "elf_symbol_versions.h"
#include "so_util.h"

#include <stdint.h>
#include <string.h>

static const char *const quest_android_externals[] = {
    "libc.so",
    "libm.so",
    "libdl.so",
    "liblog.so",
    "libandroid.so",
    "libstdc++.so",
    "libEGL.so",
    "libGLESv3.so",
    "libz.so",
    "libmediandk.so"
};

const BsElfDependencyPolicy bs_elf_dep_quest_android_policy = {
    quest_android_externals,
    sizeof quest_android_externals / sizeof quest_android_externals[0]
};

static const char *file_basename(const char *path) {
    if (!path || !*path) return NULL;
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *last = a && b ? (a > b ? a : b) : (a ? a : b);
    return last ? last + 1 : path;
}

static void copy_error_name(char *out, size_t capacity, const char *name) {
    if (!capacity) return;
    size_t n = name ? strlen(name) : 0;
    if (n >= capacity) n = capacity - 1;
    if (n) memcpy(out, name, n);
    out[n] = 0;
}

static BsElfDependencyStatus failed(BsElfDependencyError *error,
                                     BsElfDependencyStatus status,
                                     size_t module_index,
                                     const char *name) {
    if (error) {
        error->status = status;
        error->module_index = module_index;
        copy_error_name(error->dependency, sizeof error->dependency, name);
    }
    return status;
}

static int is_allowed_external(const BsElfDependencyPolicy *policy,
                               const char *soname) {
    if (!policy || !policy->allowed_external_sonames) return 0;
    for (size_t i = 0; i < policy->allowed_external_count; ++i) {
        if (!strcmp(soname, policy->allowed_external_sonames[i]))
            return 1;
    }
    return 0;
}

static int valid_policy(const BsElfDependencyPolicy *policy) {
    if (!policy) return 1; /* Fail closed on every missing dependency. */
    if (policy->allowed_external_count && !policy->allowed_external_sonames)
        return 0;
    for (size_t i = 0; i < policy->allowed_external_count; ++i) {
        const char *name = policy->allowed_external_sonames[i];
        if (!name || !*name || strcmp(name, file_basename(name)))
            return 0;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(name, policy->allowed_external_sonames[j]))
                return 0;
    }
    return 1;
}

BsElfDependencyStatus bs_elf_dependency_order_images(
    const BsElfDependencyImage *images,
    size_t count,
    const BsElfDependencyPolicy *external_policy,
    size_t *out_order,
    BsElfDependencyError *error) {
    if (error) {
        memset(error, 0, sizeof(*error));
        error->module_index = SIZE_MAX;
    }
    if (!count || !images || !out_order || !valid_policy(external_policy))
        return failed(error, BS_ELF_DEP_ARGUMENT, SIZE_MAX, NULL);
    if (count > BS_ELF_DEP_MAX_MODULES)
        return failed(error, BS_ELF_DEP_TOO_MANY, SIZE_MAX, NULL);

    /* The portable GNU parser checks ELF64 AArch64, PT_LOAD bounds, PT_DYNAMIC,
     * dynamic strings, DT_NEEDED entries, version tables and file ranges.
     * It accepts both pristine originals and precisely rebased so_load bytes.
     */
    BsElfVersionView views[BS_ELF_DEP_MAX_MODULES];
    const char *identities[BS_ELF_DEP_MAX_MODULES];
    for (size_t i = 0; i < count; ++i) {
        const BsElfDependencyImage *module = &images[i];
        const char *base = file_basename(module->filename);
        if (!base || !*base || !module->image || !module->image_size)
            return failed(error, BS_ELF_DEP_ARGUMENT, i, module->filename);
        if (bs_elf_version_view_init_rebased(&views[i],
                module->image, module->image_size,
                module->program_header_bias) != BS_ELF_VERSION_MATCH)
            return failed(error, BS_ELF_DEP_MALFORMED, i, base);
        const char *soname = views[i].soname ? views[i].soname : base;
        if (!*soname || strcmp(soname, file_basename(soname)))
            return failed(error, BS_ELF_DEP_MALFORMED, i, base);
        identities[i] = soname;
        for (size_t j = 0; j < i; ++j) {
            if (!strcmp(identities[j], identities[i]) ||
                !strcmp(file_basename(images[j].filename), base))
                return failed(error, BS_ELF_DEP_DUPLICATE, i, identities[i]);
        }
    }

    /* edges[dependent][prerequisite]. Duplicated DT_NEEDED entries are one
     * graph edge. Every unprovided SONAME must be *explicitly* allowlisted.
     */
    uint8_t edges[BS_ELF_DEP_MAX_MODULES][BS_ELF_DEP_MAX_MODULES] = {{0}};
    size_t prerequisites[BS_ELF_DEP_MAX_MODULES] = {0};
    for (size_t i = 0; i < count; ++i) {
        for (size_t d = 0; d < views[i].needed_count; ++d) {
            const char *needed = NULL;
            if (bs_elf_version_dependency(&views[i], d, &needed) !=
                    BS_ELF_VERSION_MATCH || !needed || !*needed)
                return failed(error, BS_ELF_DEP_MALFORMED, i, needed);
            size_t provider = count;
            for (size_t j = 0; j < count; ++j) {
                if (!strcmp(identities[j], needed)) {
                    provider = j;
                    break;
                }
            }
            if (provider == count) {
                if (!is_allowed_external(external_policy, needed))
                    return failed(error, BS_ELF_DEP_MISSING, i, needed);
                continue;
            }
            if (provider == i)
                return failed(error, BS_ELF_DEP_CYCLE, i, needed);
            if (!edges[i][provider]) {
                edges[i][provider] = 1;
                ++prerequisites[i];
            }
        }
    }

    /* Stable Kahn traversal: smallest staged input index wins each tie.
     * Build into scratch so on any failure the caller's output is untouched.
     */
    size_t order[BS_ELF_DEP_MAX_MODULES];
    uint8_t emitted[BS_ELF_DEP_MAX_MODULES] = {0};
    for (size_t step = 0; step < count; ++step) {
        size_t ready = count;
        for (size_t i = 0; i < count; ++i) {
            if (!emitted[i] && prerequisites[i] == 0) {
                ready = i;
                break;
            }
        }
        if (ready == count) {
            for (size_t i = 0; i < count; ++i)
                if (!emitted[i])
                    return failed(error, BS_ELF_DEP_CYCLE, i, identities[i]);
            return failed(error, BS_ELF_DEP_CYCLE, SIZE_MAX, NULL);
        }
        emitted[ready] = 1;
        order[step] = ready;
        for (size_t i = 0; i < count; ++i) {
            if (edges[i][ready]) {
                edges[i][ready] = 0;
                --prerequisites[i];
            }
        }
    }
    memcpy(out_order, order, count * sizeof(*order));
    return BS_ELF_DEP_OK;
}

BsElfDependencyStatus bs_elf_dependency_order_modules(
    const struct so_module *modules, size_t count,
    const BsElfDependencyPolicy *external_policy,
    size_t *out_order, BsElfDependencyError *error) {
    if (!count || !modules || count > BS_ELF_DEP_MAX_MODULES) {
        if (count > BS_ELF_DEP_MAX_MODULES)
            return failed(error, BS_ELF_DEP_TOO_MANY, SIZE_MAX, NULL);
        return failed(error, BS_ELF_DEP_ARGUMENT, SIZE_MAX, NULL);
    }
    BsElfDependencyImage images[BS_ELF_DEP_MAX_MODULES];
    for (size_t i = 0; i < count; ++i) {
        if (!modules[i].so_base || !modules[i].load_virtbase)
            return failed(error, BS_ELF_DEP_ARGUMENT, i, modules[i].name);
        images[i] = (BsElfDependencyImage) {
            modules[i].name,
            modules[i].so_base,
            modules[i].so_size,
            (uint64_t)(uintptr_t)modules[i].load_virtbase
        };
    }
    return bs_elf_dependency_order_images(images, count, external_policy,
                                           out_order, error);
}

const char *bs_elf_dependency_status_string(BsElfDependencyStatus status) {
    switch (status) {
        case BS_ELF_DEP_OK: return "dependency order verified";
        case BS_ELF_DEP_ARGUMENT: return "invalid dependency ordering arguments";
        case BS_ELF_DEP_MALFORMED: return "invalid original ELF metadata";
        case BS_ELF_DEP_DUPLICATE: return "duplicate staged ELF identity";
        case BS_ELF_DEP_MISSING: return "required ELF dependency unavailable";
        case BS_ELF_DEP_CYCLE: return "cyclic ELF dependency graph";
        case BS_ELF_DEP_TOO_MANY: return "too many staged ELF modules";
        default: return "unknown dependency-order error";
    }
}
