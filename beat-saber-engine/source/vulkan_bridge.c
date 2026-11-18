#include "vulkan_bridge.h"

#include <stddef.h>
#include <string.h>
#include <stdatomic.h>

static atomic_bool s_graphics_boundary_armed;
static atomic_bool s_graphics_boundary_seen;
static atomic_bool s_unity_window_enabled;

void bs_vulkan_arm_graphics_boundary(bool armed) {
    if (armed)
        atomic_store_explicit(&s_graphics_boundary_seen, false,
                              memory_order_release);
    atomic_store_explicit(&s_graphics_boundary_armed, armed, memory_order_release);
}

bool bs_vulkan_graphics_boundary_seen(void) {
    return atomic_load_explicit(&s_graphics_boundary_seen, memory_order_acquire);
}

void bs_vulkan_enable_unity_window(void) {
    bs_vulkan_arm_graphics_boundary(false);
    atomic_store_explicit(&s_unity_window_enabled, true, memory_order_release);
}

#ifdef BS_USE_NVK
#include <stdlib.h>
#include <switch.h>
#include "util.h"

#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

/* The window remains valid across Unity surface/swapchain recreation. Its
 * Horizon handles live until process exit, after all guest users have stopped. */
static Mutex s_window_lock;
static ViLayer s_unity_layer;
static NWindow s_unity_window;
static int s_window_x, s_window_y;
static int s_window_max_x, s_window_max_y;

static NWindow *unity_presentation_window(void) {
    mutexLock(&s_window_lock);
    if (nwindowIsValid(&s_unity_window)) {
        mutexUnlock(&s_window_lock);
        return &s_unity_window;
    }

    ViDisplay display = {0};
    Result rc = viOpenDefaultDisplay(&display);
    s32 display_width = 0, display_height = 0;
    if (R_SUCCEEDED(rc))
        rc = viGetDisplayLogicalResolution(&display, &display_width,
                                           &display_height);
    if (R_SUCCEEDED(rc) && (display_width < 64 || display_height < 36))
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(rc))
        rc = viCreateLayer(&display, &s_unity_layer);
    if (R_SUCCEEDED(rc) &&
        s_unity_layer.igbp_binder_obj_id == (u32)nwindowGetDefault()->bq.id)
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(rc)) {
        const s32 fit_width = display_width / 16;
        const s32 fit_height = display_height / 9;
        const s32 scale = (fit_width < fit_height ? fit_width : fit_height) * 3 / 4;
        const s32 width = scale * 16, height = scale * 9;
        s_window_max_x = display_width - width;
        s_window_max_y = display_height - height;
        s_window_x = s_window_max_x / 2;
        s_window_y = s_window_max_y / 2;
        rc = viSetLayerScalingMode(&s_unity_layer, ViScalingMode_PreserveAspectRatio);
        if (R_SUCCEEDED(rc)) rc = viSetLayerSize(&s_unity_layer, width, height);
        if (R_SUCCEEDED(rc))
            rc = viSetLayerPosition(&s_unity_layer, s_window_x, s_window_y);
        if (R_SUCCEEDED(rc))
            rc = nwindowCreateFromLayer(&s_unity_window, &s_unity_layer);
    }
    if (R_FAILED(rc)) {
        if (s_unity_layer.initialized) viCloseLayer(&s_unity_layer);
        if (display.initialized) viCloseDisplay(&display);
        debugPrintf("[graphics:window] failed to create Unity presentation layer rc=0x%x\n", rc);
    } else {
        debugPrintf("[graphics:window] Unity Vulkan presentation layer initialized\n");
    }
    mutexUnlock(&s_window_lock);
    return R_SUCCEEDED(rc) ? &s_unity_window : NULL;
}

static int move_screen_axis(int position, int direction, int limit) {
    const int next = position + direction * 16;
    if (next < 0) return 0;
    if (next > limit) return limit;
    return next;
}

static bool stop_at_graphics_boundary(const char *entrypoint) {
    if (!atomic_load_explicit(&s_graphics_boundary_armed, memory_order_acquire))
        return false;
    if (!atomic_exchange_explicit(&s_graphics_boundary_seen, true,
                                  memory_order_acq_rel))
        debugPrintf("[graphics:boundary] ORIGINAL guest reached %s; "
                    "graphics initialization stopped before driver entry\n",
                    entrypoint);
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL android_enumerate_extensions(
    const char *layer, uint32_t *count, VkExtensionProperties *properties) {
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = enumerate(layer, count, properties);
    if (result >= VK_SUCCESS && properties) {
        for (uint32_t i = 0; i < *count; i++) {
            if (strcmp(properties[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME) == 0) {
                strcpy(properties[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
                properties[i].specVersion = VK_KHR_ANDROID_SURFACE_SPEC_VERSION;
            }
        }
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL android_create_instance(
    const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkInstance *instance) {
    if (stop_at_graphics_boundary("vkCreateInstance"))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!info || !instance || info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
        (info->enabledExtensionCount && !info->ppEnabledExtensionNames))
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkInstanceCreateInfo translated = *info;
    const char **names = NULL;
    if (info->enabledExtensionCount) {
        names = calloc(info->enabledExtensionCount, sizeof(*names));
        if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
            const char *name = info->ppEnabledExtensionNames[i];
            names[i] = strcmp(name, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) == 0
                ? VK_NN_VI_SURFACE_EXTENSION_NAME : name;
        }
        translated.ppEnabledExtensionNames = names;
    }
    VkResult result = create(&translated, allocator, instance);
    free(names);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL android_create_surface(
    VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    if (stop_at_graphics_boundary("vkCreateAndroidSurfaceKHR"))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!info || !surface ||
        info->sType != VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!info->window || !nwindowIsValid((NWindow *)info->window))
        return VK_ERROR_SURFACE_LOST_KHR;
    if (info->pNext || info->flags)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    PFN_vkCreateViSurfaceNN create = (PFN_vkCreateViSurfaceNN)
        vkGetInstanceProcAddr(instance, "vkCreateViSurfaceNN");
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    NWindow *window = (NWindow *)info->window;
    if (atomic_load_explicit(&s_unity_window_enabled, memory_order_acquire)) {
        window = unity_presentation_window();
        if (!window) return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkViSurfaceCreateInfoNN translated = {
        .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
        .window = window,
    };
    return create(instance, &translated, allocator, surface);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL android_get_instance_proc(
    VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)android_get_instance_proc;
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)android_create_instance;
    if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)android_enumerate_extensions;
    if (strcmp(name, "vkCreateAndroidSurfaceKHR") == 0)
        return instance && vkGetInstanceProcAddr(instance, "vkCreateViSurfaceNN")
            ? (PFN_vkVoidFunction)android_create_surface : NULL;
    return vkGetInstanceProcAddr(instance, name);
}
#endif

void bs_vulkan_move_unity_window(int horizontal, int vertical) {
#ifdef BS_USE_NVK
    mutexLock(&s_window_lock);
    if (nwindowIsValid(&s_unity_window)) {
        const int x = move_screen_axis(s_window_x, horizontal, s_window_max_x);
        const int y = move_screen_axis(s_window_y, vertical, s_window_max_y);
        if (x != s_window_x || y != s_window_y) {
            Result rc = viSetLayerPosition(&s_unity_layer, x, y);
            if (R_SUCCEEDED(rc)) {
                s_window_x = x;
                s_window_y = y;
            } else {
                debugPrintf("[graphics:window] layer translation failed rc=0x%x\n", rc);
            }
        }
    }
    mutexUnlock(&s_window_lock);
#else
    (void)horizontal;
    (void)vertical;
#endif
}

void *bs_vulkan_symbol(const char *name) {
#ifdef BS_USE_NVK
    return (void *)android_get_instance_proc(VK_NULL_HANDLE, name);
#else
    (void)name;
    return NULL;
#endif
}

bool bs_vulkan_probe_pregraphics(void *native_window,
                                 BsVulkanPregraphicsProbe *evidence) {
    if (!evidence) return false;
    *evidence = (BsVulkanPregraphicsProbe){0};
#ifdef BS_USE_NVK
    evidence->nvk_available = true;
    evidence->window_valid = native_window &&
        nwindowIsValid((NWindow *)native_window);
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)
        vkGetInstanceProcAddr(VK_NULL_HANDLE,
                              "vkEnumerateInstanceExtensionProperties");
    evidence->instance_functions_available = create && enumerate;
    if (!enumerate) {
        evidence->enumeration_result = VK_ERROR_INITIALIZATION_FAILED;
        return false;
    }
    uint32_t count = 0;
    VkResult result = enumerate(NULL, &count, NULL);
    evidence->enumeration_result = (int32_t)result;
    evidence->extension_count = count;
    if (result != VK_SUCCESS || !count || count > 4096)
        return false;
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) {
        evidence->enumeration_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        return false;
    }
    result = enumerate(NULL, &count, extensions);
    evidence->enumeration_result = (int32_t)result;
    evidence->extension_count = count;
    if (result == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!strcmp(extensions[i].extensionName,
                        VK_NN_VI_SURFACE_EXTENSION_NAME))
                evidence->vi_surface_extension = true;
            if (!strcmp(extensions[i].extensionName,
                        VK_KHR_SURFACE_EXTENSION_NAME))
                evidence->khr_surface_extension = true;
        }
    }
    free(extensions);
    return evidence->nvk_available &&
        evidence->instance_functions_available &&
        evidence->window_valid && evidence->vi_surface_extension &&
        evidence->khr_surface_extension && result == VK_SUCCESS;
#else
    (void)native_window;
    return false;
#endif
}
