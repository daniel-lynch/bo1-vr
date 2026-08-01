/* experiments/05_submit/vkcheck.c
 *
 * Cross-checks vk_min.h against the real Vulkan headers. Not built by `all`,
 * because it needs libvulkan-dev; run `make vkcheck` after touching vk_min.h.
 *
 * <vulkan/vulkan_core.h> is platform-independent C (it pulls in nothing but
 * stdint/stddef and vk_platform.h), so it compiles fine under the i686 mingw
 * cross compiler with -I/usr/include, which is what the Makefile does. Note
 * this is deliberately the ONLY translation unit allowed to see /usr/include.
 */
#include <vulkan/vulkan_core.h>
#include "vk_min.h"

#define SAME_SIZE(ours, theirs) \
    _Static_assert(sizeof(ours) == sizeof(theirs), #ours " size != " #theirs)
#define SAME_OFF(ours, theirs, field) \
    _Static_assert(__builtin_offsetof(ours, field) == __builtin_offsetof(theirs, field), \
                   #ours "." #field " offset != " #theirs "." #field)

/* Handle widths. The non-dispatchable/dispatchable asymmetry on i386 is the
 * whole reason this check exists. */
SAME_SIZE(bo1vr_VkImage,          VkImage);
SAME_SIZE(bo1vr_VkInstance,       VkInstance);
SAME_SIZE(bo1vr_VkPhysicalDevice, VkPhysicalDevice);
SAME_SIZE(bo1vr_VkDevice,         VkDevice);
SAME_SIZE(bo1vr_VkQueue,          VkQueue);
_Static_assert(sizeof(VkImage) == 8, "VkImage must be 64-bit even on i386");
_Static_assert(sizeof(VkDevice) == 4, "VkDevice must be a 32-bit pointer here");

SAME_SIZE(bo1vr_VkExtent3D, VkExtent3D);
SAME_OFF(bo1vr_VkExtent3D, VkExtent3D, width);
SAME_OFF(bo1vr_VkExtent3D, VkExtent3D, height);
SAME_OFF(bo1vr_VkExtent3D, VkExtent3D, depth);

SAME_SIZE(bo1vr_VkImageSubresourceRange, VkImageSubresourceRange);
SAME_OFF(bo1vr_VkImageSubresourceRange, VkImageSubresourceRange, aspectMask);
SAME_OFF(bo1vr_VkImageSubresourceRange, VkImageSubresourceRange, baseMipLevel);
SAME_OFF(bo1vr_VkImageSubresourceRange, VkImageSubresourceRange, levelCount);
SAME_OFF(bo1vr_VkImageSubresourceRange, VkImageSubresourceRange, baseArrayLayer);
SAME_OFF(bo1vr_VkImageSubresourceRange, VkImageSubresourceRange, layerCount);

SAME_SIZE(bo1vr_VkImageCreateInfo, VkImageCreateInfo);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, sType);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, pNext);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, flags);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, imageType);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, format);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, extent);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, mipLevels);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, arrayLayers);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, samples);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, tiling);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, usage);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, sharingMode);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, queueFamilyIndexCount);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, pQueueFamilyIndices);
SAME_OFF(bo1vr_VkImageCreateInfo, VkImageCreateInfo, initialLayout);

/* Enumerant values we hardcode. */
_Static_assert(BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "");
_Static_assert(BO1VR_VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "");
_Static_assert(BO1VR_VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, "");
_Static_assert(BO1VR_VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "");
_Static_assert(BO1VR_VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, "");
_Static_assert(BO1VR_VK_IMAGE_ASPECT_COLOR_BIT == VK_IMAGE_ASPECT_COLOR_BIT, "");
_Static_assert(BO1VR_VK_IMAGE_USAGE_TRANSFER_SRC_BIT == VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "");
_Static_assert(BO1VR_VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "");
_Static_assert(BO1VR_VK_FORMAT_B8G8R8A8_UNORM == VK_FORMAT_B8G8R8A8_UNORM, "");
_Static_assert(BO1VR_VK_FORMAT_B8G8R8A8_SRGB  == VK_FORMAT_B8G8R8A8_SRGB, "");
_Static_assert(BO1VR_VK_FORMAT_R8G8B8A8_UNORM == VK_FORMAT_R8G8B8A8_UNORM, "");
_Static_assert(BO1VR_VK_FORMAT_R8G8B8A8_SRGB  == VK_FORMAT_R8G8B8A8_SRGB, "");

int bo1vr_vkcheck_ok = 1;
