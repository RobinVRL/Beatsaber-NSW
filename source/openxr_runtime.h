/* In-process OpenXR runtime for Switch (see openxr_runtime.c).
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_OPENXR_RUNTIME_H
#define BS_OPENXR_RUNTIME_H

#include "so_util.h"

/* Branch-hook every runtime-implemented export of the staged (still
 * writable) libopenxr_loader.so. Call before so_finalize. */
int bs_openxr_hook_loader(so_module *loader);
/* Make OVRPlugin report user presence and app input/VR focus. */
int bs_openxr_hook_ovrplugin(so_module *ovrp);
/* Runtime entry point for an OpenXR function name, or NULL. */
void *bs_openxr_symbol(const char *name);
/* Seated head orientation, from the loader's L + D-pad control. */
void bs_openxr_set_head_angles(int yaw_degrees, int pitch_degrees);

#endif
