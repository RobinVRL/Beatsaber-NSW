#ifndef BS_VULKAN_BRIDGE_H
#define BS_VULKAN_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* Android libvulkan.so exports provided by the optional Switch NVK driver. */
void *bs_vulkan_symbol(const char *name);

/* Query the actual driver/VI window without creating VkInstance, VkSurface,
 * swapchain, graphics queue or rendering resources. This reports whether a
 * future graphics handoff has the minimum native capabilities, NOT whether
 * Vulkan/OpenXR initialization has run successfully. */
typedef struct BsVulkanPregraphicsProbe {
    bool nvk_available;
    bool instance_functions_available;
    bool window_valid;
    bool vi_surface_extension;
    bool khr_surface_extension;
    uint32_t extension_count;
    int32_t enumeration_result;
} BsVulkanPregraphicsProbe;
bool bs_vulkan_probe_pregraphics(void *native_window,
                                 BsVulkanPregraphicsProbe *evidence);

/* Arm immediately before entering original Quest guest code. The first guest
 * Vulkan instance/surface request is logged and rejected before entering the
 * graphics driver; this lets the port verify its requested stop boundary
 * without starting rendering. Mere symbol lookup never trips the boundary. */
void bs_vulkan_arm_graphics_boundary(bool armed);
bool bs_vulkan_graphics_boundary_seen(void);

/* Opt into routing a genuine Android Vulkan surface to a single Horizon
 * layer. Motion is a translation of that layer in logical display space. */
void bs_vulkan_enable_unity_window(void);
void bs_vulkan_move_unity_window(int horizontal, int vertical);

#endif
