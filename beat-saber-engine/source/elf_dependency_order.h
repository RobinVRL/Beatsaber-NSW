/* Deterministic, verified constructor ordering for original Quest ELF modules.
 * Copyright (c) 2026 Beat Saber NX contributors. MIT license.
 *
 * No constructors or other guest code are called. Graph membership is NOT
 * proof of Android ABI, JNI, GPU, audio or Unity lifecycle readiness.
 */
#ifndef BS_ELF_DEPENDENCY_ORDER_H
#define BS_ELF_DEPENDENCY_ORDER_H

#include <stddef.h>
#include <stdint.h>

struct so_module;

/* Hard cap on modules for bounded work/stack use on libnx. */
#define BS_ELF_DEP_MAX_MODULES 64u

typedef enum BsElfDependencyStatus {
    BS_ELF_DEP_OK = 0,
    BS_ELF_DEP_ARGUMENT = -1,
    BS_ELF_DEP_MALFORMED = -2,
    BS_ELF_DEP_DUPLICATE = -3,
    BS_ELF_DEP_MISSING = -4,
    BS_ELF_DEP_CYCLE = -5,
    BS_ELF_DEP_TOO_MANY = -6
} BsElfDependencyStatus;

typedef struct BsElfDependencyError {
    BsElfDependencyStatus status;
    size_t module_index;       /* SIZE_MAX if not attributable to one module */
    char dependency[128];      /* missing SONAME, duplicate identity or cycle */
} BsElfDependencyError;

/* Explicit and exact external SONAME allowlist. Never a promise that the
 * native compatibility bridge actually implements any exported functions.
 * A NULL policy means NO external dependencies may be ignored.
 */
typedef struct BsElfDependencyPolicy {
    const char *const *allowed_external_sonames;
    size_t allowed_external_count;
} BsElfDependencyPolicy;

/* Built-in policy matching ONLY the non-staged DT_NEEDED SONAMEs observed in
 * the original 15 Quest ELF files. Passing this explicitly authorizes
 * excluding them from graph edges; it does not authorize entering guest code.
 */
extern const BsElfDependencyPolicy bs_elf_dep_quest_android_policy;

/* Preloaded or pristine AArch64 ELF64. The image bytes must stay alive for the
 * duration of this call; program_header_bias=0 for original disk bytes.
 * When so_load rebased PT_* p_vaddr, pass its mod->load_virtbase as the bias.
 */
typedef struct BsElfDependencyImage {
    const char *filename;
    const void *image;
    size_t image_size;
    uint64_t program_header_bias;
} BsElfDependencyImage;

/* Returns BS_ELF_DEP_OK and a complete permutation of [0,count) such that
 * every staged dependency precedes its dependent. Ties use input index order.
 * On failure, out_order is UNCHANGED and error is filled if non-NULL.
 * Rejects missing non-allowlisted DT_NEEDED, duplicate SONAMEs, self/cyclic
 * dependency graphs, malformed ELF and ambiguous/missing source identities.
 */
BsElfDependencyStatus bs_elf_dependency_order_images(
    const BsElfDependencyImage *images,
    size_t count,
    const BsElfDependencyPolicy *external_policy,
    size_t *out_order,
    BsElfDependencyError *error);

/* Convenience integration with the existing so_module modules[count] array.
 * CALL ONLY AFTER ALL so_load AND BEFORE so_free_temp (which frees so_base).
 * Uses original staging file bytes and the exact so_load p_vaddr rebase.
 * Never requires changing main's module staging or linked list.
 */
BsElfDependencyStatus bs_elf_dependency_order_modules(
    const struct so_module *modules,
    size_t count,
    const BsElfDependencyPolicy *external_policy,
    size_t *out_order,
    BsElfDependencyError *error);

const char *bs_elf_dependency_status_string(BsElfDependencyStatus status);

#endif
