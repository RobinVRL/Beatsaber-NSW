#include "../source/joycon_xr_actions.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

int main(void) {
    BsJoyconSnapshot snapshot = {0};
    BsJoyconXrValue value = {0};
    BsJoyconHandSample *left = &snapshot.hands[BS_JOYCON_HAND_LEFT];
    BsJoyconHandSample *right = &snapshot.hands[BS_JOYCON_HAND_RIGHT];
    snapshot.poll_timestamp_ns = 123456;
    left->connected = 1;
    left->capabilities = BS_JOYCON_CAP_BUTTONS | BS_JOYCON_CAP_STICK;
    left->trigger = 1.0f;
    left->grip = 1.0f;
    left->primary_pressed = 1;
    left->stick[0] = -0.75f;
    left->stick[1] = 0.5f;
    left->connection_epoch = 9;
    right->connected = 1;
    right->capabilities = BS_JOYCON_CAP_BUTTONS;
    right->secondary_pressed = 1;
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/left/input/trigger/value", &value) == 0);
    assert(value.kind == BS_JOYCON_XR_FLOAT && value.active &&
           value.scalar == 1.0f && value.connection_epoch == 9);
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/left/input/thumbstick", &value) == 0);
    assert(value.kind == BS_JOYCON_XR_VECTOR2 && value.active &&
           value.vector2[0] == -0.75f && value.vector2[1] == 0.5f);
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/right/input/b/click", &value) == 0);
    assert(value.kind == BS_JOYCON_XR_BOOLEAN && value.scalar == 1.0f);
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/right/input/a/click", &value) == 0);
    assert(value.active && value.scalar == 0.0f);
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/right/input/thumbstick", &value) == 0);
    assert(!value.active);
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/left/input/grip/pose", &value) == 0);
    assert(value.kind == BS_JOYCON_XR_POSE && !value.active &&
           !value.relative_orientation_available);
    left->capabilities |= BS_JOYCON_CAP_RELATIVE_ORIENTATION;
    left->orientation_xyzw[3] = 1.0f;
    left->motion_timestamp_ns = 45678;
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/left/input/aim/pose", &value) == 0);
    assert(value.relative_orientation_available &&
           value.relative_orientation_xyzw[3] == 1.0f &&
           value.timestamp_ns == 45678);
    left->connected = 0;
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/left/input/trigger/value", &value) == 0);
    assert(!value.active && value.scalar == 0.0f);
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/left/input/a/click", &value) == -ENOENT);
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/right/input/trigger/unknown", &value) == -ENOENT);
    assert(bs_joycon_xr_resolve_binding(NULL,
        "/user/hand/left/input/trigger/value", &value) == -EINVAL);
    assert(bs_joycon_xr_resolve_binding(&snapshot, NULL, &value) == -EINVAL);
    assert(bs_joycon_xr_resolve_binding(&snapshot,
        "/user/hand/left/input/trigger/value", NULL) == -EINVAL);
    return 0;
}
