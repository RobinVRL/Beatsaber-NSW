/* Static ELF TLS for the original Quest ARM64 PT_TLS modules.
 * SPDX-License-Identifier: MIT
 *
 * The Quest package is fully staged before game entry, so a bounded static
 * layout can service its TLSDESC relocations without Android's dynamic DTV.
 * Late dlopen of a new TLS module must fail until dynamic TLS is implemented.
 */
#ifndef BS_GUEST_TLS_H
#define BS_GUEST_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <elf.h>

struct so_module;

/* ABI: two adjacent 64-bit GOT words, resolver followed by argument. */
typedef struct BsGuestTlsDesc {
    uintptr_t resolver;
    intptr_t argument;
} BsGuestTlsDesc;

typedef struct BsGuestTlsIndex {
    uintptr_t module_id;
    uintptr_t offset;
} BsGuestTlsIndex;

bool bs_guest_tls_register_module(struct so_module *module);
bool bs_guest_tls_seal(void);
/* Called after RELA/RELR is complete but before any guest thread is attached.
 * PT_TLS .tdata can itself contain RELR relocations in the original APK. */
bool bs_guest_tls_snapshot_relocated_templates(void);
void bs_guest_tls_late_snapshot(const struct so_module *module);
bool bs_guest_tls_is_ready(void);
bool bs_guest_tls_is_thread_attached(void);
bool bs_guest_tls_apply_desc(struct so_module *module, const Elf64_Rela *rela);
/* Validate a REAL, already relocated two-word TLSDESC in the FINALIZED
 * executable mapping (load_virtbase), using this native bridge's resolver.
 * desc_vaddr is the original virtual offset of the descriptor in module;
 * tls_var_offset is the expected module-local PT_TLS byte offset.
 * Requires so_finalize() to have rebased dynsym/dynstr, the current thread
 * attached, and its Bionic stack guard installed. The donated source
 * load_base is inaccessible after finalization and must never be read here.
 * No guest engine code or constructors are executed.
 */
bool bs_guest_tls_validate_original_desc(const struct so_module *module,
                                         uintptr_t desc_vaddr,
                                         size_t tls_var_offset);
size_t bs_guest_tls_relocation_count(void);
size_t bs_guest_tls_module_count(void);
/* Return module ID as expected by __tls_get_addr; 0 means no PT_TLS. */
uintptr_t bs_guest_tls_module_id(const struct so_module *module);

/* Attach a caller-owned, 16-byte-aligned BIONIC_TLS_SIZE buffer to the
 * current native thread after bs_guest_tls_seal(). Returns 0 on success or a
 * POSIX error number. On failure TPIDR_EL0 is left untouched. Only one
 * guest TLS attachment per native thread is permitted.
 *
 * The caller must keep buffer alive until after bs_guest_tls_detach_thread().
 * Worker threads should detach AFTER guest pthread-key destructors, while
 * their native thread is still alive.
 */
int bs_guest_tls_attach_thread(void *buffer);
void bs_guest_tls_detach_thread(void);

/* Compatibility entrypoint used by main.c. Same requirements as attach. */
bool bs_guest_tls_install_current_thread(void *buffer);

/* Bionic __tls_get_addr uses module_id/offset; null means unsupported. */
void *bs_guest_tls_get_addr(const BsGuestTlsIndex *index);

/* AArch64 TLSDESC resolver: preserves all registers other than x0, x1, x30.
 * Its argument is a pointer to the descriptor in x0, per AAELF64.
 */
intptr_t bs_guest_tls_static_resolver(const BsGuestTlsDesc *descriptor);

#endif
