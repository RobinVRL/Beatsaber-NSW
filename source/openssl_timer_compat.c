/* Quest libcrypto.so uses a SIGILL-protected ARM64 CPU-feature probe. Eden's
 * Dynarmic decoder aborts at MRS x0, CNTVCT_EL0 before the installed SIGILL
 * handler can run. Horizon's libnx reads the real physical timer CNTPCT_EL0,
 * whose instruction differs by exactly one 32-bit opcode.
 *
 * Preserve the original constructor, sigsetjmp, branch and feature checks.
 * The only modification to the writable staged guest image is:
 *   0x000f8b48: d53be040 (mrs x0,cntvct_el0)
 *            -> d53be020 (mrs x0,cntpct_el0)
 *
 * The physical counter may have an offset relative to the virtual counter;
 * this is suitable for the feature/timing probe, not a claim that the two
 * timer domains are globally interchangeable.
 */
#include "openssl_timer_compat.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <switch/crypto/sha256.h>

enum {
    BS_CRYPTO_ORIGINAL_FILE_SIZE = 2368784,
    BS_CRYPTO_CONTEXT_VA = 0x000f8b38,
    BS_CRYPTO_OPCODE_VA = 0x000f8b48,
    BS_CRYPTO_PROBE_VA = 0x000f8db8,
};

/* SHA256 of the unmodified Beat Saber Quest APK lib/arm64-v8a/libcrypto.so.
 * The staged private-SD copy has the same hash. The digest is calculated on
 * the preserved so_base file image, never on relocated code or SD filenames. */
static const uint8_t original_sha256[SHA256_HASH_SIZE] = {
    0x0c, 0x68, 0x64, 0x2b, 0x32, 0xb4, 0xd1, 0x49,
    0x69, 0x5b, 0x09, 0xcd, 0xee, 0x88, 0x42, 0x7d,
    0x13, 0x6e, 0x68, 0x4d, 0xe7, 0x79, 0xab, 0x77,
    0x58, 0xdd, 0xe6, 0x74, 0x2f, 0xca, 0x9a, 0x88
};

/* Consecutive original instructions 0xf8b38..0xf8b54 inclusive:
 * ret; udf; mov; ret; mrs x0,cntvct_el0; ret; aese; ret.
 * Check neighboring feature probes to rule out a coincidental MRS opcode. */
static const uint8_t original_context[] = {
    0xc0, 0x03, 0x5f, 0xd6, 0x00, 0x00, 0x00, 0x00,
    0xef, 0x1d, 0xaf, 0x4e, 0xc0, 0x03, 0x5f, 0xd6,
    0x40, 0xe0, 0x3b, 0xd5, 0xc0, 0x03, 0x5f, 0xd6,
    0x00, 0x48, 0x28, 0x4e, 0xc0, 0x03, 0x5f, 0xd6
};

/* Original ctor instructions 0xf8db8..0xf8dd4:
 * ADRP, ADD, MOV, BL sigsetjmp, CBNZ, BL 0xf8b48, LDR, ORR.
 * The direct BL proves that this exact guarded probe uses our target. */
static const uint8_t original_probe[] = {
    0x60, 0x0a, 0x00, 0x90, 0x00, 0x60, 0x33, 0x91,
    0x21, 0x00, 0x80, 0x52, 0xbf, 0x7d, 0x04, 0x94,
    0xa0, 0x00, 0x00, 0x35, 0x5f, 0xff, 0xff, 0x97,
    0xc8, 0x02, 0x40, 0xb9, 0x08, 0x01, 0x1f, 0x32
};

static const uint8_t physical_counter_opcode[] = {
    0x20, 0xe0, 0x3b, 0xd5 /* d53be020: mrs x0,cntpct_el0 */
};

static bool original_executable_range(const so_module *module,
                                      uintptr_t address, size_t length) {
    if (!module || address > module->load_size ||
        length > module->load_size - address ||
        module->phnum < 1 || module->phnum > SO_MAX_SEGMENTS * 2)
        return false;
    for (int i = 0; i < module->phnum; ++i) {
        const Elf64_Phdr *ph = &module->phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) ||
            address < ph->p_vaddr) continue;
        const uint64_t offset = (uint64_t)address - ph->p_vaddr;
        if (offset <= ph->p_filesz && length <= ph->p_filesz - offset)
            return true;
    }
    return false;
}

int bs_openssl_timer_compat_patch(so_module *module) {
    if (!module || !module->so_base || !module->load_base ||
        !module->load_virtbase) return -EINVAL;
    if (strcmp(module->name, "sdmc:/switch/beat-saber-1408/libcrypto.so") &&
        strcmp(module->name, "libcrypto.so"))
        return -ENOEXEC;
    if (module->so_size != BS_CRYPTO_ORIGINAL_FILE_SIZE ||
        !original_executable_range(module, BS_CRYPTO_CONTEXT_VA,
                                   sizeof original_context) ||
        !original_executable_range(module, BS_CRYPTO_PROBE_VA,
                                   sizeof original_probe))
        return -ENOEXEC;

    uint8_t actual_hash[SHA256_HASH_SIZE];
    sha256CalculateHash(actual_hash, module->so_base, module->so_size);
    if (memcmp(actual_hash, original_sha256, sizeof original_sha256) != 0)
        return -ENOEXEC;

    /* Test all original bytes BEFORE the first write. A repeated invocation
     * is rejected because the original virtual-counter opcode is now absent.
     * This avoids silently applying a patch to a different/stale ELF image. */
    uint8_t *const staged = module->load_base;
    if (memcmp(staged + BS_CRYPTO_CONTEXT_VA, original_context,
               sizeof original_context) ||
        memcmp(staged + BS_CRYPTO_PROBE_VA, original_probe,
               sizeof original_probe))
        return -EILSEQ;

    memcpy(staged + BS_CRYPTO_OPCODE_VA, physical_counter_opcode,
           sizeof physical_counter_opcode);
    return 0;
}
