#include "joycon_xr_actions.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

typedef enum BsActionField {
    BS_ACTION_TRIGGER,
    BS_ACTION_GRIP,
    BS_ACTION_PRIMARY,
    BS_ACTION_SECONDARY,
    BS_ACTION_MENU,
    BS_ACTION_STICK_CLICK,
    BS_ACTION_STICK,
    BS_ACTION_POSE
} BsActionField;

typedef struct BsActionBinding {
    const char *suffix;
    BsJoyconXrValueKind kind;
    BsActionField field;
    unsigned sides;
} BsActionBinding;

enum { BS_SIDE_L = 1, BS_SIDE_R = 2, BS_SIDE_BOTH = 3 };

/* Oculus Touch-style paths used by Unity OpenXR, with KHR simple-controller
 * select and grip/aim pose aliases. Values derive solely from physical HID. */
static const BsActionBinding g_bindings[] = {
    {"trigger/value", BS_JOYCON_XR_FLOAT, BS_ACTION_TRIGGER, BS_SIDE_BOTH},
    {"trigger/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_TRIGGER, BS_SIDE_BOTH},
    {"squeeze/value", BS_JOYCON_XR_FLOAT, BS_ACTION_GRIP, BS_SIDE_BOTH},
    {"squeeze/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_GRIP, BS_SIDE_BOTH},
    {"select/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_TRIGGER, BS_SIDE_BOTH},
    {"x/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_PRIMARY, BS_SIDE_L},
    {"y/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_SECONDARY, BS_SIDE_L},
    {"a/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_PRIMARY, BS_SIDE_R},
    {"b/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_SECONDARY, BS_SIDE_R},
    {"menu/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_MENU, BS_SIDE_BOTH},
    {"thumbstick", BS_JOYCON_XR_VECTOR2, BS_ACTION_STICK, BS_SIDE_BOTH},
    {"thumbstick/click", BS_JOYCON_XR_BOOLEAN, BS_ACTION_STICK_CLICK, BS_SIDE_BOTH},
    {"grip/pose", BS_JOYCON_XR_POSE, BS_ACTION_POSE, BS_SIDE_BOTH},
    {"aim/pose", BS_JOYCON_XR_POSE, BS_ACTION_POSE, BS_SIDE_BOTH}
};

int bs_joycon_xr_resolve_binding(const BsJoyconSnapshot *snapshot,
                                const char *binding_path,
                                BsJoyconXrValue *value) {
    if (!snapshot || !binding_path || !value) return -EINVAL;
    memset(value, 0, sizeof(*value));
    const char *suffix;
    unsigned hand;
    unsigned side;
    static const char left_prefix[] = "/user/hand/left/input/";
    static const char right_prefix[] = "/user/hand/right/input/";
    if (strncmp(binding_path, left_prefix, sizeof(left_prefix) - 1) == 0) {
        hand = BS_JOYCON_HAND_LEFT;
        side = BS_SIDE_L;
        suffix = binding_path + sizeof(left_prefix) - 1;
    } else if (strncmp(binding_path, right_prefix,
                       sizeof(right_prefix) - 1) == 0) {
        hand = BS_JOYCON_HAND_RIGHT;
        side = BS_SIDE_R;
        suffix = binding_path + sizeof(right_prefix) - 1;
    } else {
        return -ENOENT;
    }
    const BsActionBinding *binding = NULL;
    for (size_t i = 0; i < sizeof(g_bindings) / sizeof(g_bindings[0]); ++i) {
        if ((g_bindings[i].sides & side) &&
            strcmp(suffix, g_bindings[i].suffix) == 0) {
            binding = &g_bindings[i];
            break;
        }
    }
    if (!binding) return -ENOENT;
    const BsJoyconHandSample *source = &snapshot->hands[hand];
    value->kind = binding->kind;
    value->connection_epoch = source->connection_epoch;
    value->timestamp_ns = binding->field == BS_ACTION_POSE ?
                          source->motion_timestamp_ns : snapshot->poll_timestamp_ns;
    unsigned caps = source->capabilities;
    if (binding->field == BS_ACTION_POSE) {
        value->relative_orientation_available = source->connected &&
            !!(caps & BS_JOYCON_CAP_RELATIVE_ORIENTATION);
        value->active = value->relative_orientation_available;
        if (value->active)
            memcpy(value->relative_orientation_xyzw, source->orientation_xyzw,
                   sizeof(value->relative_orientation_xyzw));
        return 0;
    }
    value->active = source->connected &&
        !!(caps & (binding->field == BS_ACTION_STICK ?
                   BS_JOYCON_CAP_STICK : BS_JOYCON_CAP_BUTTONS));
    if (!value->active) return 0;
    switch (binding->field) {
        case BS_ACTION_TRIGGER: value->scalar = source->trigger; break;
        case BS_ACTION_GRIP: value->scalar = source->grip; break;
        case BS_ACTION_PRIMARY: value->scalar = !!source->primary_pressed; break;
        case BS_ACTION_SECONDARY: value->scalar = !!source->secondary_pressed; break;
        case BS_ACTION_MENU: value->scalar = !!source->menu_pressed; break;
        case BS_ACTION_STICK_CLICK: value->scalar = !!source->stick_pressed; break;
        case BS_ACTION_STICK:
            memcpy(value->vector2, source->stick, sizeof(value->vector2));
            break;
        case BS_ACTION_POSE: break;
    }
    return 0;
}
