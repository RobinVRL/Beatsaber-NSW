#pragma once

/* Log fields/methods of IL2CPP classes whose name contains any filter
 * (NULL-terminated list). Diagnostic; call on a thread that may attach. */
void bs_il2cpp_probe(const char *const *filters);

/* Log Unity XR devices at Head/LeftHand/RightHand nodes. */
void bs_il2cpp_probe_xr_devices(void);

/* Dump live instances (fields from +0x18, GameObject active) of classes. */
void bs_il2cpp_probe_objects(const char *const *class_names);

/* Log Input System devices and their last update times. */
void bs_il2cpp_probe_input_system(void);

/* Log UnityXRHelper controller InputActions (bindings, enabled, value). */
void bs_il2cpp_probe_helper_actions(void);
