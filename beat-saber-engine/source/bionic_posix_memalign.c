/* Android/NDK posix_memalign ABI backed by newlib's aligned allocator.
 * Return an error number rather than setting errno, and leave the caller's
 * pointer untouched when allocation fails, as POSIX requires.
 */
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>

/* Keep this guest ABI entrypoint distinct from NVK's host allocator, which
 * also defines posix_memalign in libvulkan.a. The symbol bridge explicitly
 * chooses this function for guest Bionic imports. */
int bs_bionic_posix_memalign(void **result, size_t alignment, size_t bytes) {
    /* POSIX declares result nonnull; callers violating that contract have
     * undefined behavior. Checking it here triggers -Wnonnull-compare. */
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;
    void *storage = memalign(alignment, bytes ? bytes : 1);
    if (!storage) return ENOMEM;
    *result = storage;
    return 0;
}
