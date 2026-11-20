/* Game-facing OpenXR binding semantics for two Nintendo Joy-Cons.
 * This module converts actual HID samples to action values. The hosting
 * OpenXR runtime owns XrAction/XrPath/XrSpace handles and synchronization.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_JOYCON_XR_ACTIONS_H
#define BS_JOYCON_XR_ACTIONS_H

#include "joycon_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum BsJoyconXrValueKind {
    BS_JOYCON_XR_BOOLEAN = 1,
    BS_JOYCON_XR_FLOAT = 2,
    BS_JOYCON_XR_VECTOR2 = 3,
    BS_JOYCON_XR_POSE = 4
} BsJoyconXrValueKind;

typedef struct BsJoyconXrValue {
    BsJoyconXrValueKind kind;
    int active;
    /* Position remains unavailable. Pose orientation is expressed in the
     * uncalibrated Nintendo sensor frame, and must not be returned as a valid
     * OpenXR reference-space pose until a runtime applies its calibration. */
    int relative_orientation_available;
    float scalar;
    float vector2[2];
    float relative_orientation_xyzw[4];
    uint64_t connection_epoch;
    int64_t timestamp_ns;
} BsJoyconXrValue;

/* Accepts an OpenXR user path such as
 * /user/hand/left/input/trigger/value or /user/hand/right/input/aim/pose.
 * Returns 0 for known mappings (with active=0 on disconnect), -ENOENT for
 * an unknown binding, and -EINVAL for invalid parameters. A return of 0
 * does not claim a running XR session or tracked/valid world-space pose. */
int bs_joycon_xr_resolve_binding(const BsJoyconSnapshot *snapshot,
                                const char *binding_path,
                                BsJoyconXrValue *value);

#ifdef __cplusplus
}
#endif

#endif
