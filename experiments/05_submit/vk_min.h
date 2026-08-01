/* experiments/05_submit/vk_min.h
 *
 * The *minimum* slice of the Vulkan API needed to talk to DXVK's D3D9 <-> Vulkan
 * interop and to fill in OpenVR's VRVulkanTextureData_t. Hand-written so this
 * experiment has no build dependency on libvulkan-dev.
 *
 * Every declaration here is layout-critical: DXVK writes through these pointers.
 * `make vkcheck` compiles vkcheck.c, which includes BOTH this header and the
 * system <vulkan/vulkan_core.h> and _Static_assert()s that the sizes and the
 * offsets agree. Run it whenever this file is touched. It is not part of `all`
 * because it needs libvulkan-dev, which the rest of the experiment does not.
 *
 * NOTE on handle widths: Vulkan's NON-dispatchable handles (VkImage) are
 * uint64_t on every platform, including 32-bit. Dispatchable handles
 * (VkInstance, VkDevice, VkPhysicalDevice, VkQueue) are pointers, so they are
 * 4 bytes in our 32-bit PE. That asymmetry is real and both DXVK and Proton's
 * vrclient agree on it -- see RESULTS.md.
 */
#ifndef BO1VR_VK_MIN_H
#define BO1VR_VK_MIN_H

#include <stdint.h>

typedef struct VkInstance_T       *bo1vr_VkInstance;
typedef struct VkPhysicalDevice_T *bo1vr_VkPhysicalDevice;
typedef struct VkDevice_T         *bo1vr_VkDevice;
typedef struct VkQueue_T          *bo1vr_VkQueue;
typedef uint64_t                   bo1vr_VkImage;   /* non-dispatchable: 64-bit even on i386 */

typedef uint32_t bo1vr_VkFlags;
typedef int32_t  bo1vr_VkEnum;

/* VkImageLayout */
#define BO1VR_VK_IMAGE_LAYOUT_UNDEFINED                 0
#define BO1VR_VK_IMAGE_LAYOUT_GENERAL                   1
#define BO1VR_VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL  2
#define BO1VR_VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL  5
#define BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL      6
#define BO1VR_VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL      7
#define BO1VR_VK_IMAGE_LAYOUT_PRESENT_SRC_KHR           1000001002

/* VkStructureType */
#define BO1VR_VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO       14

/* VkImageAspectFlagBits */
#define BO1VR_VK_IMAGE_ASPECT_COLOR_BIT                 0x00000001u

/* VkImageUsageFlagBits -- only the ones we report on */
#define BO1VR_VK_IMAGE_USAGE_TRANSFER_SRC_BIT           0x00000001u
#define BO1VR_VK_IMAGE_USAGE_TRANSFER_DST_BIT           0x00000002u
#define BO1VR_VK_IMAGE_USAGE_SAMPLED_BIT                0x00000004u
#define BO1VR_VK_IMAGE_USAGE_STORAGE_BIT                0x00000008u
#define BO1VR_VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT       0x00000010u

/* A few VkFormat values, for legible logging only. */
#define BO1VR_VK_FORMAT_R8G8B8A8_UNORM                  37
#define BO1VR_VK_FORMAT_R8G8B8A8_SRGB                   43
#define BO1VR_VK_FORMAT_B8G8R8A8_UNORM                  44
#define BO1VR_VK_FORMAT_B8G8R8A8_SRGB                   50

typedef struct bo1vr_VkExtent3D {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} bo1vr_VkExtent3D;

typedef struct bo1vr_VkImageSubresourceRange {
    bo1vr_VkFlags aspectMask;
    uint32_t      baseMipLevel;
    uint32_t      levelCount;
    uint32_t      baseArrayLayer;
    uint32_t      layerCount;
} bo1vr_VkImageSubresourceRange;

typedef struct bo1vr_VkImageCreateInfo {
    bo1vr_VkEnum      sType;
    const void       *pNext;
    bo1vr_VkFlags     flags;
    bo1vr_VkEnum      imageType;
    bo1vr_VkEnum      format;
    bo1vr_VkExtent3D  extent;
    uint32_t          mipLevels;
    uint32_t          arrayLayers;
    bo1vr_VkEnum      samples;
    bo1vr_VkEnum      tiling;
    bo1vr_VkFlags     usage;
    bo1vr_VkEnum      sharingMode;
    uint32_t          queueFamilyIndexCount;
    const uint32_t   *pQueueFamilyIndices;
    bo1vr_VkEnum      initialLayout;
} bo1vr_VkImageCreateInfo;

#endif /* BO1VR_VK_MIN_H */
