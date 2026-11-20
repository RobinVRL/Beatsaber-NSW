/* Exact, opt-in in-memory compatibility patch for the staged Quest OpenSSL.
 * This changes one counter register read in a real libcrypto constructor
 * probe. It neither bypasses constructors nor touches the on-disk ELF.
 */
#ifndef BS_OPENSSL_TIMER_COMPAT_H
#define BS_OPENSSL_TIMER_COMPAT_H

#include "so_util.h"

/* Apply ONLY to the SHA256-pinned original libcrypto.so before so_finalize().
 * The caller must have completed so_relocate/so_resolve and must still own
 * the writable load_base and intact, unmodified so_base.
 *
 * Returns 0 after changing precisely one A64 instruction, or negative errno
 * without modifying anything if the source identity, opcode, call site,
 * executable PT_LOAD layout, or mapped bytes differ from the pinned image.
 *
 * Existing so_finalize + so_flush_caches publish the patched instruction;
 * do not call so_patch_code or write the RX load_virtbase directly.
 */
int bs_openssl_timer_compat_patch(so_module *module);

#endif
