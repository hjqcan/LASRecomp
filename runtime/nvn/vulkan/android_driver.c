// Android: which Vulkan driver the backend uses. LARECOMP_VK_DRIVER=<path of a driver library in the app's internal
// storage> (the activity sets it from the player's driver package, e.g. Turnip): loaded through libadrenotools, in a
// linker namespace of its own (its hook libraries are in nativeLibraryDir, LARECOMP_NATIVE_LIB_DIR). Otherwise, or when
// that fails or offers no GPU, the system's driver. volk then takes every Vulkan function from it (device.c).
// From LASRecompAndroid by mamcjr (MIT). (Empty elsewhere: the desktop builds compile every file here.)
#ifdef __ANDROID__
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"

// Does this driver offer a Vulkan GPU at all? (Turnip v26.0.0 R8, built for the A7xx: none on an Adreno 830 - the app
// stopped at "no Vulkan device" on every start, its settings out of reach.)
static bool driver_has_gpu(PFN_vkGetInstanceProcAddr gipa) {
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!create) return false;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (create(&ci, NULL, &inst) != VK_SUCCESS) return false;
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    bool ok = false;
    uint32_t n = 0;
    if (enumerate && props && enumerate(inst, &n, NULL) == VK_SUCCESS && n) {
        VkPhysicalDevice pd[8];
        if (n > 8) n = 8;
        if (enumerate(inst, &n, pd) >= 0)
            for (uint32_t i = 0; i < n && !ok; i++) {
                VkPhysicalDeviceProperties p;
                props(pd[i], &p);
                ok = p.apiVersion >= VK_API_VERSION_1_3;
            }
    }
    if (destroy) destroy(inst, NULL);
    return ok;
}

void vk_android_load(void) {
    void *lib = NULL;
    const char *drv = getenv("LARECOMP_VK_DRIVER"), *hooks = getenv("LARECOMP_NATIVE_LIB_DIR");
    if (drv && *drv && hooks && *hooks) {
        const char *slash = strrchr(drv, '/');
        char dir[1024];
        if (slash && (size_t)snprintf(dir, sizeof dir, "%.*s/", (int)(slash - drv), drv) < sizeof dir)
            lib = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, dir, slash + 1,
                                             NULL, NULL);
        LOG("[vk] driver %s: %s\n", drv, lib ? "loaded through adrenotools" : "FAILED, using the system driver");
        PFN_vkGetInstanceProcAddr g = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
        if (lib && (!g || !driver_has_gpu(g))) {
            LOG("[vk] driver %s: no Vulkan 1.3 GPU it supports here - using the system driver\n", drv);
            lib = NULL;  // (left loaded: a driver doesn't always unload cleanly)
        }
    }
    // (what is in use, for the start screen's Graphics driver row: the package's library, or "" for the system's)
    setenv("LARECOMP_VK_DRIVER_IN_USE", lib && drv ? (strrchr(drv, '/') ? strrchr(drv, '/') + 1 : drv) : "", 1);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) FATAL("no Vulkan driver: %s\n", dlerror());
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) FATAL("the Vulkan driver has no vkGetInstanceProcAddr\n");
    volkInitializeCustom(gipa);
}
#endif
