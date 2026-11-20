#ifndef BS_EGL_BRIDGE_H
#define BS_EGL_BRIDGE_H

#include <stdint.h>

/* Guest-facing EGL/GLES entry for a symbol name, or NULL. EGL entries are
 * Android-translating wrappers; GLES entries are Mesa's own functions. */
void *bs_egl_symbol(const char *name);
/* Number of eglSwapBuffers calls made by the guest. */
uint64_t bs_egl_frames_presented(void);
/* Draw a (layer of a) GLES texture rect to the guest window surface and swap.
 * tw/th: full texture size. Needs the guest context current. 0 on success. */
int bs_egl_present_texture(unsigned tex, int layer, int is_array,
                           int sx, int sy, int sw, int sh, int tw, int th);

/* Create and destroy a context on the host main thread (diagnostic). */
int bs_egl_host_preflight(void);

#endif
