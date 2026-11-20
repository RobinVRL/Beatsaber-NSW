#ifndef BS_PLATFORM_BRIDGE_H
#define BS_PLATFORM_BRIDGE_H
#include "so_util.h"

/* A small, ABI-compatible portion of Android's C system imports. This table
 * is intentionally incomplete: it is used to measure remaining missing
 * native dependencies. Android C++/pthread/FILE/JNI/graphics interfaces require
 * genuine compatibility implementations before engine execution is enabled.
 */
extern DynLibFunction bs_platform_symbols[];
extern const int bs_platform_symbols_count;
/* Point guest EGL/GLES imports at the EGL bridge; call before so_resolve. */
void bs_platform_bind_graphics(void);
#endif
