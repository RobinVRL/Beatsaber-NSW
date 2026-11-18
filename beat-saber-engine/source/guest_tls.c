/* Static AArch64 ELF TLS layout for genuine Quest ARM64 libraries.
 * SPDX-License-Identifier: MIT
 */
#include <switch.h>
#include <elf.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "guest_tls.h"
#include "so_util.h"
#include "util.h"

#define BS_TLS_MODULE_CAP 32
/* Leave the Android TLS slots, including the guard at TP+0x28, alone. */
#define BS_TLS_FIRST_MODULE_OFFSET 0x100u
#define BS_TLS_REQUIRED_ALIGNMENT 16u

typedef struct BsTlsModule {
    const so_module *module;
    uintptr_t module_id;
    size_t tp_offset;
    size_t memsz;
    size_t filesz;
} BsTlsModule;

static BsTlsModule g_modules[BS_TLS_MODULE_CAP];
static size_t g_modules_count;
static size_t g_next_offset = BS_TLS_FIRST_MODULE_OFFSET;
static size_t g_descriptor_count;
static bool g_sealed;
static bool g_templates_ready;
static bool g_tls_key_ready;
static pthread_key_t g_tls_key;
static uint8_t g_template[BIONIC_TLS_SIZE];
typedef struct BsTlsThreadState {
    uint8_t *buffer;
    void *native_tp;
} BsTlsThreadState;

static bool power_of_two(size_t value) {
    return value && !(value & (value - 1));
}

static bool align_within(size_t value, size_t alignment, size_t *out) {
    if (!power_of_two(alignment) || value > SIZE_MAX - (alignment - 1))
        return false;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

bool bs_guest_tls_register_module(so_module *module) {
    if (!module || !module->load_base || g_sealed) return false;
    for (size_t i = 0; i < g_modules_count; ++i)
        if (g_modules[i].module == module) return true;
    for (int i = 0; i < module->phnum; ++i) {
        const Elf64_Phdr *ph = &module->phdr[i];
        if (ph->p_type != PT_TLS) continue;
        if (g_modules_count == BS_TLS_MODULE_CAP ||
            ph->p_memsz < ph->p_filesz ||
            ph->p_vaddr > module->load_size ||
            ph->p_memsz > module->load_size - ph->p_vaddr ||
            ph->p_align > BS_TLS_REQUIRED_ALIGNMENT ||
            !power_of_two(ph->p_align ? ph->p_align : 1))
            return false;
        size_t offset;
        if (!align_within(g_next_offset, ph->p_align ? ph->p_align : 1,
                          &offset) ||
            offset > BIONIC_TLS_SIZE - BIONIC_TLS_TP_OFFSET ||
            ph->p_memsz > BIONIC_TLS_SIZE - BIONIC_TLS_TP_OFFSET - offset)
            return false;

        /* This PT_TLS image may contain RELR relocation targets. Delay the
         * copy until so_relocate() has finished for EVERY original module.
         * libOVRPlugin.so's 8-byte initializer is a real RELR target. */
        g_modules[g_modules_count] = (BsTlsModule){
            .module = module,
            .module_id = g_modules_count + 1,
            .tp_offset = offset,
            .memsz = (size_t)ph->p_memsz,
            .filesz = (size_t)ph->p_filesz,
        };
        ++g_modules_count;
        g_next_offset = offset + (size_t)ph->p_memsz;
        debugPrintf("[tls] %s PT_TLS module=%lu offset=0x%lx filesz=%lu memsz=%lu align=%lu\n",
                    module->name, (unsigned long)g_modules_count,
                    (unsigned long)offset, (unsigned long)ph->p_filesz,
                    (unsigned long)ph->p_memsz, (unsigned long)ph->p_align);
        return true;
    }
    return true;
}

bool bs_guest_tls_seal(void) {
    if (g_sealed) return true;
    /* Must be created before guest worker threads can run concurrently. */
    if (pthread_key_create(&g_tls_key, NULL)) return false;
    g_tls_key_ready = true;
    g_sealed = true;
    return true;
}

bool bs_guest_tls_snapshot_relocated_templates(void) {
    if (!g_sealed) return false;
    if (g_templates_ready) return true;
    for (size_t i = 0; i < g_modules_count; ++i) {
        const BsTlsModule *tls = &g_modules[i];
        const so_module *module = tls->module;
        if (!module || !module->load_base) return false;
        const Elf64_Phdr *segment = NULL;
        for (int p = 0; p < module->phnum; ++p)
            if (module->phdr[p].p_type == PT_TLS) {
                segment = &module->phdr[p];
                break;
            }
        if (!segment || tls->filesz != segment->p_filesz ||
            segment->p_vaddr > module->load_size ||
            tls->filesz > module->load_size - segment->p_vaddr)
            return false;
        if (tls->filesz)
            memcpy(g_template + BIONIC_TLS_TP_OFFSET + tls->tp_offset,
                   (const uint8_t *)module->load_base + segment->p_vaddr,
                   tls->filesz);
        debugPrintf("[tls] copied relocated .tdata %s (%lu bytes)\n",
                    module->name, (unsigned long)tls->filesz);
    }
    g_templates_ready = true;
    return true;
}

bool bs_guest_tls_is_ready(void) {
    return g_sealed && g_tls_key_ready && g_templates_ready;
}

bool bs_guest_tls_is_thread_attached(void) {
    if (!bs_guest_tls_is_ready()) return false;
    const BsTlsThreadState *thread = pthread_getspecific(g_tls_key);
    return thread && thread->buffer &&
           armGetTlsRw() == thread->buffer + BIONIC_TLS_TP_OFFSET;
}

size_t bs_guest_tls_module_count(void) { return g_modules_count; }
uintptr_t bs_guest_tls_module_id(const so_module *module) {
    if (!module) return 0;
    for (size_t i = 0; i < g_modules_count; ++i)
        if (g_modules[i].module == module) return g_modules[i].module_id;
    return 0;
}
size_t bs_guest_tls_relocation_count(void) { return g_descriptor_count; }

/* The caller owns the storage. The guest TLS key owns only metadata; Bionic
 * pthread-key callbacks must finish before we restore TPIDR_EL0 and clear
 * that metadata. This is intentionally NOT a key destructor: native POSIX
 * key-destructor ordering does not guarantee that guest callbacks run first.
 */
int bs_guest_tls_attach_thread(void *buffer) {
    if (!buffer || ((uintptr_t)buffer & (BS_TLS_REQUIRED_ALIGNMENT - 1)))
        return EINVAL;
    if (!bs_guest_tls_is_ready()) return ENOTSUP;
    if (pthread_getspecific(g_tls_key)) return EBUSY;

    BsTlsThreadState *thread = calloc(1, sizeof(*thread));
    if (!thread) return ENOMEM;
    thread->buffer = buffer;
    thread->native_tp = armGetTlsRw();

    /* The TP-relative header is an Android AArch64 TCB. Leave the 0x100
     * bytes reserved for Bionic thread slots separate from PT_TLS payloads.
     * In particular TP+0x28 is the stack guard, written by
     * bs_bionic_tls_finalize_current_thread() AFTER attachment.
     */
    memset(buffer, 0, BIONIC_TLS_SIZE);
    memcpy((uint8_t *)buffer + BIONIC_TLS_TP_OFFSET + BS_TLS_FIRST_MODULE_OFFSET,
           g_template + BIONIC_TLS_TP_OFFSET + BS_TLS_FIRST_MODULE_OFFSET,
           g_next_offset - BS_TLS_FIRST_MODULE_OFFSET);
    int rc = pthread_setspecific(g_tls_key, thread);
    if (rc) {
        free(thread);
        return rc;
    }
    armSetTlsRw((uint8_t *)buffer + BIONIC_TLS_TP_OFFSET);
    return 0;
}

bool bs_guest_tls_install_current_thread(void *buffer) {
    return bs_guest_tls_attach_thread(buffer) == 0;
}

void bs_guest_tls_detach_thread(void) {
    if (!bs_guest_tls_is_ready()) return;
    BsTlsThreadState *thread = pthread_getspecific(g_tls_key);
    if (!thread) return;
    /* Clear the key before touching TPIDR_EL0; this prevents lookups during
     * native teardown from seeing an attachment that is no longer active. */
    pthread_setspecific(g_tls_key, NULL);
    if (armGetTlsRw() == thread->buffer + BIONIC_TLS_TP_OFFSET)
        armSetTlsRw(thread->native_tp);
    free(thread);
}

void *bs_guest_tls_get_addr(const BsGuestTlsIndex *index) {
    if (!index || !bs_guest_tls_is_ready() ||
        index->module_id == 0 || index->module_id > g_modules_count)
        return NULL;
    const BsTlsThreadState *thread = pthread_getspecific(g_tls_key);
    if (!thread || !thread->buffer ||
        armGetTlsRw() != thread->buffer + BIONIC_TLS_TP_OFFSET)
        return NULL;
    const BsTlsModule *module = &g_modules[index->module_id - 1];
    if (index->offset >= module->memsz) return NULL;
    return thread->buffer +
           BIONIC_TLS_TP_OFFSET + module->tp_offset + index->offset;
}

/* Android AArch64's static resolver is exactly these two instructions.
 * Unlike a normal C function it must preserve x2-x29 and all SIMD registers.
 */
__asm__(".text\n"
        ".p2align 2\n"
        ".global bs_guest_tls_static_resolver\n"
        ".type bs_guest_tls_static_resolver,%function\n"
        "bs_guest_tls_static_resolver:\n"
        "ldr x0, [x0, #8]\n"
        "ret\n"
        ".size bs_guest_tls_static_resolver, . - bs_guest_tls_static_resolver\n");

bool bs_guest_tls_apply_desc(so_module *module, const Elf64_Rela *rela) {
    if (!module || !rela || !g_sealed || !module->load_base ||
        ELF64_R_TYPE(rela->r_info) != R_AARCH64_TLSDESC ||
        !module->syms || module->num_syms <= 0 ||
        rela->r_offset > module->load_size ||
        module->load_size - rela->r_offset < sizeof(BsGuestTlsDesc))
        return false;
    const size_t symbol_index = ELF64_R_SYM(rela->r_info);
    if (symbol_index >= (size_t)module->num_syms) return false;
    const Elf64_Sym *symbol = &module->syms[symbol_index];

    /* All 15 R_AARCH64_TLSDESC in the provided Quest package have STN_UNDEF
     * (index zero) and their RELA addend is the module's .tbss offset.
     * A later package with cross-DSO TLS references needs dependency-aware
     * dynamic TLS, and must fail explicitly instead of binding another DSO.
     */
    if (symbol_index != STN_UNDEF &&
        (symbol->st_shndx == SHN_UNDEF ||
         ELF64_ST_TYPE(symbol->st_info) != STT_TLS)) return false;
    const BsTlsModule *tls = NULL;
    for (size_t i = 0; i < g_modules_count; ++i)
        if (g_modules[i].module == module) { tls = &g_modules[i]; break; }
    if (!tls || rela->r_addend < 0) return false;
    const uint64_t symoff = symbol_index == STN_UNDEF ? 0 : symbol->st_value;
    if (symoff > tls->memsz ||
        (uint64_t)rela->r_addend >= tls->memsz - symoff)
        return false;
    const uintptr_t target_offset = tls->tp_offset + symoff + (uint64_t)rela->r_addend;
    BsGuestTlsDesc *descriptor = (BsGuestTlsDesc *)((uintptr_t)module->load_base + rela->r_offset);
    descriptor->resolver = (uintptr_t)&bs_guest_tls_static_resolver;
    descriptor->argument = (intptr_t)target_offset;
    ++g_descriptor_count;
    return true;
}

bool bs_guest_tls_validate_original_desc(const so_module *module,
                                         uintptr_t desc_vaddr,
                                         size_t tls_var_offset) {
    /* so_finalize() donates load_base's pages to the runtime mapping.
     * It also rebases dynsym and dynstr to load_virtbase only AFTER mapping
     * and permission setup succeed. These pointers are the loader's current
     * observable finalization marker; merely having load_virtbase set is
     * insufficient because that VA was reserved back in so_load().
     */
    if (!module || !module->load_virtbase || !module->load_base ||
        !module->syms || !module->dynstrtab || !module->load_size ||
        (uintptr_t)module->load_virtbase > UINTPTR_MAX - module->load_size ||
        (uintptr_t)module->load_base == (uintptr_t)module->load_virtbase ||
        (uintptr_t)module->syms < (uintptr_t)module->load_virtbase ||
        (uintptr_t)module->syms - (uintptr_t)module->load_virtbase >=
            module->load_size ||
        (uintptr_t)module->dynstrtab < (uintptr_t)module->load_virtbase ||
        (uintptr_t)module->dynstrtab - (uintptr_t)module->load_virtbase >=
            module->load_size ||
        !bs_guest_tls_is_thread_attached() ||
        (desc_vaddr & (sizeof(uintptr_t) - 1)) ||
        desc_vaddr > module->load_size ||
        module->load_size - desc_vaddr < sizeof(BsGuestTlsDesc))
        goto invalid;

    const BsTlsModule *tls = NULL;
    for (size_t i = 0; i < g_modules_count; ++i)
        if (g_modules[i].module == module) { tls = &g_modules[i]; break; }
    if (!tls || tls_var_offset >= tls->memsz)
        goto invalid;

    const uintptr_t tp = (uintptr_t)armGetTlsRw();
    if (!tp || ((const uintptr_t *)tp)[5] == 0 ||
        tls->tp_offset < BS_TLS_FIRST_MODULE_OFFSET ||
        tls->tp_offset > BIONIC_TLS_SIZE - BIONIC_TLS_TP_OFFSET ||
        tls_var_offset > BIONIC_TLS_SIZE - BIONIC_TLS_TP_OFFSET -
                         tls->tp_offset)
        goto invalid;

    /* This validator is called AFTER so_finalize. svcMapProcessCodeMemory
     * donates the staging load_base pages, which must not be dereferenced
     * thereafter. Read the live relocated GOT through its accessible
     * load_virtbase mapping, as the original Quest code will do. */
    const BsGuestTlsDesc *descriptor =
        (const BsGuestTlsDesc *)((uintptr_t)module->load_virtbase + desc_vaddr);
    if (descriptor->resolver != (uintptr_t)&bs_guest_tls_static_resolver)
        goto invalid;

    const intptr_t expected_offset = (intptr_t)(tls->tp_offset + tls_var_offset);
    if (descriptor->argument != expected_offset)
        goto invalid;

    /* The actual AArch64 static resolver must return exactly the second
     * word from the original, already patched Quest GOT descriptor. */
    const intptr_t offset = bs_guest_tls_static_resolver(descriptor);
    if (offset != expected_offset || offset < 0 ||
        (uintptr_t)offset >= BIONIC_TLS_SIZE - BIONIC_TLS_TP_OFFSET ||
        tp > UINTPTR_MAX - (uintptr_t)offset)
        goto invalid;

    const BsGuestTlsIndex index = {tls->module_id, tls_var_offset};
    if ((uintptr_t)bs_guest_tls_get_addr(&index) != tp + (uintptr_t)offset)
        goto invalid;
    return true;

invalid:
    debugPrintf("[tls] original TLSDESC validation rejected %s at 0x%lx "
                "TLS+0x%lx\n",
                module ? module->name : "(null)",
                (unsigned long)desc_vaddr,
                (unsigned long)tls_var_offset);
    return false;
}

