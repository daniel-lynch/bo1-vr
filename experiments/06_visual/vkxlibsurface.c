/* experiments/06_visual/vkxlibsurface.c
 *
 * A small Vulkan layer that removes the two -- and only two -- things standing
 * between this machine and an on-screen view of Monado's compositor output.
 *
 * WHY THIS EXISTS
 *
 * Monado can show its own composited output in an ordinary desktop window:
 * `XRT_WINDOW_PEEK=both|left|right`, src/xrt/compositor/main/comp_window_peek.c.
 * That is exactly the observation channel Experiment 5 lacked. Enabling it on
 * this machine hits two unrelated environment mismatches, both fatal, neither
 * configurable:
 *
 * DEFECT 1 -- the peek surface cannot be created.
 *
 *   ERROR [comp_window_peek_create] Failed to create SDL surface:
 *         VK_KHR_xlib_surface extension is not enabled in the Vulkan instance.
 *
 *   Monado picks its instance extensions from the *window backend* it selected;
 *   with X11(XCB) that list has VK_KHR_xcb_surface and not VK_KHR_xlib_surface
 *   (comp_compositor.c: select_instance_extensions()). But the peek window is an
 *   SDL2 window, and SDL2's X11 Vulkan backend commits to Xlib whenever the ICD
 *   advertises VK_KHR_xlib_surface -- SDL_x11vulkan.c only falls back to xcb when
 *   the xlib surface extension is absent from
 *   vkEnumerateInstanceExtensionProperties(). The NVIDIA ICD here advertises
 *   both, so SDL calls vkCreateXlibSurfaceKHR against an instance that did not
 *   enable it. There is no SDL hint to force the xcb path.
 *
 *   Fix: append VK_KHR_xlib_surface to vkCreateInstance.
 *
 * DEFECT 2 -- the peek swapchain cannot be created.
 *
 *   ERROR [check_surface_present_mode] Requested present mode not supported.
 *   ERROR [comp_window_peek_blit] comp_target_acquire: VK_ERROR_INITIALIZATION_FAILED
 *
 *   comp_window_peek asks for VK_PRESENT_MODE_MAILBOX_KHR. The NVIDIA driver on
 *   X11 does not offer it; measured with vulkaninfo on this machine, an X11
 *   surface reports exactly FIFO, FIFO_RELAXED, IMMEDIATE and
 *   FIFO_LATEST_READY. Monado checks the requested mode against that list and
 *   refuses before it ever calls vkCreateSwapchainKHR.
 *
 *   Fix: report MAILBOX as available, and quietly substitute IMMEDIATE when a
 *   swapchain actually asks for it. Both are tearing-permitted non-blocking
 *   modes; for a debug mirror window the difference is invisible.
 *
 * Nothing else is touched. Every other entry point is a straight pass-through,
 * and the layer is scoped to the one process that needs it -- run.sh sets
 * VK_LAYER_PATH and VK_INSTANCE_LAYERS for monado-service only, so no other
 * Vulkan application on the machine ever loads it and nothing is installed
 * system-wide.
 *
 * Build: cc -shared -fPIC -o libVkLayer_bo1vr_xlib_surface.so vkxlibsurface.c
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define LAYER_NAME "VK_LAYER_BO1VR_xlib_surface"
#define XLIB_EXT   "VK_KHR_xlib_surface"

static PFN_vkGetInstanceProcAddr g_next_gipa;
static PFN_vkGetDeviceProcAddr   g_next_gdpa;
static VkInstance                g_instance;

static void note(const char *fmt, ...)
{
    va_list ap;
    if (getenv("BO1VR_LAYER_QUIET")) return;
    va_start(ap, fmt);
    fprintf(stderr, "[" LAYER_NAME "] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static VkLayerInstanceCreateInfo *find_instance_link(const VkInstanceCreateInfo *ci)
{
    VkLayerInstanceCreateInfo *p = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO))
        p = (VkLayerInstanceCreateInfo *)p->pNext;
    return p;
}

static VkLayerDeviceCreateInfo *find_device_link(const VkDeviceCreateInfo *ci)
{
    VkLayerDeviceCreateInfo *p = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO))
        p = (VkLayerDeviceCreateInfo *)p->pNext;
    return p;
}

static VKAPI_ATTR VkResult VKAPI_CALL
bo1vr_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *link = find_instance_link(pCreateInfo);
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance      down;
    VkInstanceCreateInfo      ci;
    const char              **names = NULL;
    uint32_t                  i, n;
    int                       already = 0;
    VkResult                  r;

    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;   /* hand the next layer its link */

    down = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!down) return VK_ERROR_INITIALIZATION_FAILED;

    ci = *pCreateInfo;
    n  = ci.enabledExtensionCount;
    for (i = 0; i < n; i++)
        if (!strcmp(ci.ppEnabledExtensionNames[i], XLIB_EXT)) already = 1;

    if (!already) {
        names = malloc(sizeof(*names) * (n + 1));
        if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (i = 0; i < n; i++) names[i] = ci.ppEnabledExtensionNames[i];
        names[n] = XLIB_EXT;
        ci.ppEnabledExtensionNames = names;
        ci.enabledExtensionCount   = n + 1;
    }

    r = down(&ci, pAllocator, pInstance);
    note("vkCreateInstance: %u extension(s) requested, %s -> %d",
         n, already ? XLIB_EXT " already present" : "appended " XLIB_EXT, (int)r);
    free(names);

    if (r == VK_SUCCESS) { g_next_gipa = gipa; g_instance = *pInstance; }
    return r;
}

/* We touch nothing at device level, but a layer that appears in the instance
 * chain must still forward the device chain correctly. */
static VKAPI_ATTR VkResult VKAPI_CALL
bo1vr_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDevice *pDevice)
{
    VkLayerDeviceCreateInfo *link = find_device_link(pCreateInfo);
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr   gdpa;
    PFN_vkCreateDevice        down;
    VkResult                  r;

    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    down = (PFN_vkCreateDevice)gipa(g_instance, "vkCreateDevice");
    if (!down) return VK_ERROR_INITIALIZATION_FAILED;

    r = down(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (r == VK_SUCCESS) g_next_gdpa = gdpa;
    return r;
}

/* DEFECT 2, half one: tell Monado that MAILBOX exists. Two-call protocol:
 * pPresentModes == NULL is a count query. */
static VKAPI_ATTR VkResult VKAPI_CALL
bo1vr_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                              uint32_t *pCount, VkPresentModeKHR *pModes)
{
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR down =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
            g_next_gipa(g_instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    VkPresentModeKHR *all;
    uint32_t avail = 0, total, copy, i;
    int have_mailbox = 0;
    VkResult r;

    if (!down) return VK_ERROR_INITIALIZATION_FAILED;

    r = down(pd, surface, &avail, NULL);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;

    all = malloc(sizeof(*all) * (avail + 1));
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = down(pd, surface, &avail, all);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { free(all); return r; }

    for (i = 0; i < avail; i++)
        if (all[i] == VK_PRESENT_MODE_MAILBOX_KHR) have_mailbox = 1;
    total = avail;
    if (!have_mailbox) all[total++] = VK_PRESENT_MODE_MAILBOX_KHR;

    if (!pModes) { *pCount = total; free(all); return VK_SUCCESS; }

    copy = *pCount < total ? *pCount : total;
    memcpy(pModes, all, sizeof(*all) * copy);
    *pCount = copy;
    free(all);
    return copy < total ? VK_INCOMPLETE : VK_SUCCESS;
}

/* DEFECT 2, half two: and then do not actually ask the driver for it. */
static VKAPI_ATTR VkResult VKAPI_CALL
bo1vr_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    PFN_vkCreateSwapchainKHR down =
        (PFN_vkCreateSwapchainKHR)g_next_gdpa(device, "vkCreateSwapchainKHR");
    VkSwapchainCreateInfoKHR ci;

    if (!down) return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->presentMode != VK_PRESENT_MODE_MAILBOX_KHR)
        return down(device, pCreateInfo, pAllocator, pSwapchain);

    ci = *pCreateInfo;
    ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    note("vkCreateSwapchainKHR %ux%u: MAILBOX -> IMMEDIATE",
         ci.imageExtent.width, ci.imageExtent.height);
    return down(device, &ci, pAllocator, pSwapchain);
}

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
bo1vr_GetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)bo1vr_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)bo1vr_CreateSwapchainKHR;
    return g_next_gdpa ? g_next_gdpa(device, name) : NULL;
}

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
bo1vr_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)bo1vr_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance"))      return (PFN_vkVoidFunction)bo1vr_CreateInstance;
    if (!strcmp(name, "vkCreateDevice"))        return (PFN_vkVoidFunction)bo1vr_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr"))   return (PFN_vkVoidFunction)bo1vr_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateSwapchainKHR"))  return (PFN_vkVoidFunction)bo1vr_CreateSwapchainKHR;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR"))
        return (PFN_vkVoidFunction)bo1vr_GetPhysicalDeviceSurfacePresentModesKHR;
    return g_next_gipa ? g_next_gipa(instance, name) : NULL;
}

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr       = bo1vr_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr         = bo1vr_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
