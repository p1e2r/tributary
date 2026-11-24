// SPDX-License-Identifier: MIT
// Tributary's own declarations of the part of the Vulkan 1.0 API it uses (plus the surface
// and swapchain extensions). Written from the Vulkan specification; layouts and values are
// checked against the official headers by tests/vk_layout_check.cpp in CI.
//
// Everything is loaded at runtime through vkGetInstanceProcAddr, so only types are needed.
// Define TRIBUTARY_VK_NAMESPACE to put the declarations in a namespace (used by the check).
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(_WIN64)
#define TRIB_VKAPI __stdcall
#else
#define TRIB_VKAPI
#endif

#ifdef TRIBUTARY_VK_NAMESPACE
namespace TRIBUTARY_VK_NAMESPACE {
#endif

#define TRIB_VK_HANDLE(name) typedef struct name##_T* name;
TRIB_VK_HANDLE(VkInstance)
TRIB_VK_HANDLE(VkPhysicalDevice)
TRIB_VK_HANDLE(VkDevice)
TRIB_VK_HANDLE(VkQueue)
TRIB_VK_HANDLE(VkCommandBuffer)
TRIB_VK_HANDLE(VkSemaphore)
TRIB_VK_HANDLE(VkFence)
TRIB_VK_HANDLE(VkDeviceMemory)
TRIB_VK_HANDLE(VkBuffer)
TRIB_VK_HANDLE(VkImage)
TRIB_VK_HANDLE(VkImageView)
TRIB_VK_HANDLE(VkShaderModule)
TRIB_VK_HANDLE(VkPipelineCache)
TRIB_VK_HANDLE(VkPipelineLayout)
TRIB_VK_HANDLE(VkRenderPass)
TRIB_VK_HANDLE(VkPipeline)
TRIB_VK_HANDLE(VkDescriptorSetLayout)
TRIB_VK_HANDLE(VkSampler)
TRIB_VK_HANDLE(VkDescriptorPool)
TRIB_VK_HANDLE(VkDescriptorSet)
TRIB_VK_HANDLE(VkFramebuffer)
TRIB_VK_HANDLE(VkCommandPool)
TRIB_VK_HANDLE(VkBufferView)
TRIB_VK_HANDLE(VkSurfaceKHR)
TRIB_VK_HANDLE(VkSwapchainKHR)
#undef TRIB_VK_HANDLE

#ifndef VK_NULL_HANDLE
#define VK_NULL_HANDLE nullptr
#endif

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkSampleMask;

constexpr VkBool32 VK_FALSE = 0, VK_TRUE = 1;
constexpr uint32_t VK_QUEUE_FAMILY_IGNORED = ~0U;
constexpr uint32_t VK_SUBPASS_EXTERNAL = ~0U;
constexpr uint32_t VK_API_VERSION_1_0 = 1u << 22;
#define VK_KHR_SWAPCHAIN_EXTENSION_NAME "VK_KHR_swapchain"
#define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME "VK_KHR_portability_enumeration"

#define TRIB_VK_ENUM_END(name) name##_MAX_ENUM = 0x7FFFFFFF

enum VkResult { VK_SUCCESS = 0, VK_NOT_READY = 1, VK_TIMEOUT = 2, VK_INCOMPLETE = 5, VK_ERROR_OUT_OF_HOST_MEMORY = -1,
                VK_ERROR_OUT_OF_DEVICE_MEMORY = -2, VK_ERROR_INITIALIZATION_FAILED = -3, VK_ERROR_DEVICE_LOST = -4,
                VK_SUBOPTIMAL_KHR = 1000001003, VK_ERROR_OUT_OF_DATE_KHR = -1000001004, TRIB_VK_ENUM_END(VK_RESULT) };

enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
    VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19,
    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22,
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24,
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26,
    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27,
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
    VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37,
    VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
    VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
    VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
    VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR = 1000004000,
    VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000,
    VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT = 1000217000,
    TRIB_VK_ENUM_END(VK_STRUCTURE_TYPE)
};

enum VkFormat { VK_FORMAT_UNDEFINED = 0, VK_FORMAT_R8_UNORM = 9, VK_FORMAT_R8G8B8A8_UNORM = 37, VK_FORMAT_B8G8R8A8_UNORM = 44,
                VK_FORMAT_B8G8R8A8_SRGB = 50, VK_FORMAT_R32G32B32A32_SFLOAT = 109, TRIB_VK_ENUM_END(VK_FORMAT) };
enum VkColorSpaceKHR { VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0, TRIB_VK_ENUM_END(VK_COLOR_SPACE) };
enum VkPresentModeKHR { VK_PRESENT_MODE_IMMEDIATE_KHR = 0, VK_PRESENT_MODE_MAILBOX_KHR = 1, VK_PRESENT_MODE_FIFO_KHR = 2,
                        TRIB_VK_ENUM_END(VK_PRESENT_MODE) };
enum VkImageLayout { VK_IMAGE_LAYOUT_UNDEFINED = 0, VK_IMAGE_LAYOUT_GENERAL = 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002, TRIB_VK_ENUM_END(VK_IMAGE_LAYOUT) };
enum VkAttachmentLoadOp { VK_ATTACHMENT_LOAD_OP_LOAD = 0, VK_ATTACHMENT_LOAD_OP_CLEAR = 1, VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2,
                          TRIB_VK_ENUM_END(VK_ATTACHMENT_LOAD_OP) };
enum VkAttachmentStoreOp { VK_ATTACHMENT_STORE_OP_STORE = 0, VK_ATTACHMENT_STORE_OP_DONT_CARE = 1, TRIB_VK_ENUM_END(VK_ATTACHMENT_STORE_OP) };
enum VkBlendFactor { VK_BLEND_FACTOR_ZERO = 0, VK_BLEND_FACTOR_ONE = 1, VK_BLEND_FACTOR_SRC_ALPHA = 6,
                     VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7, TRIB_VK_ENUM_END(VK_BLEND_FACTOR) };
enum VkBlendOp { VK_BLEND_OP_ADD = 0, TRIB_VK_ENUM_END(VK_BLEND_OP) };
enum VkLogicOp { VK_LOGIC_OP_CLEAR = 0, VK_LOGIC_OP_COPY = 3, TRIB_VK_ENUM_END(VK_LOGIC_OP) };
enum VkCommandBufferLevel { VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0, TRIB_VK_ENUM_END(VK_COMMAND_BUFFER_LEVEL) };
enum VkCompareOp { VK_COMPARE_OP_NEVER = 0, VK_COMPARE_OP_ALWAYS = 7, TRIB_VK_ENUM_END(VK_COMPARE_OP) };
enum VkDescriptorType { VK_DESCRIPTOR_TYPE_SAMPLER = 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1, TRIB_VK_ENUM_END(VK_DESCRIPTOR_TYPE) };
enum VkDynamicState { VK_DYNAMIC_STATE_VIEWPORT = 0, VK_DYNAMIC_STATE_SCISSOR = 1, TRIB_VK_ENUM_END(VK_DYNAMIC_STATE) };
enum VkFilter { VK_FILTER_NEAREST = 0, VK_FILTER_LINEAR = 1, TRIB_VK_ENUM_END(VK_FILTER) };
enum VkFrontFace { VK_FRONT_FACE_COUNTER_CLOCKWISE = 0, VK_FRONT_FACE_CLOCKWISE = 1, TRIB_VK_ENUM_END(VK_FRONT_FACE) };
enum VkImageTiling { VK_IMAGE_TILING_OPTIMAL = 0, VK_IMAGE_TILING_LINEAR = 1, TRIB_VK_ENUM_END(VK_IMAGE_TILING) };
enum VkImageType { VK_IMAGE_TYPE_1D = 0, VK_IMAGE_TYPE_2D = 1, TRIB_VK_ENUM_END(VK_IMAGE_TYPE) };
enum VkImageViewType { VK_IMAGE_VIEW_TYPE_1D = 0, VK_IMAGE_VIEW_TYPE_2D = 1, TRIB_VK_ENUM_END(VK_IMAGE_VIEW_TYPE) };
enum VkComponentSwizzle { VK_COMPONENT_SWIZZLE_IDENTITY = 0, TRIB_VK_ENUM_END(VK_COMPONENT_SWIZZLE) };
enum VkPhysicalDeviceType { VK_PHYSICAL_DEVICE_TYPE_OTHER = 0, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
                            VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2, VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
                            VK_PHYSICAL_DEVICE_TYPE_CPU = 4, TRIB_VK_ENUM_END(VK_PHYSICAL_DEVICE_TYPE) };
enum VkPipelineBindPoint { VK_PIPELINE_BIND_POINT_GRAPHICS = 0, TRIB_VK_ENUM_END(VK_PIPELINE_BIND_POINT) };
enum VkPolygonMode { VK_POLYGON_MODE_FILL = 0, TRIB_VK_ENUM_END(VK_POLYGON_MODE) };
enum VkPrimitiveTopology { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3, TRIB_VK_ENUM_END(VK_PRIMITIVE_TOPOLOGY) };
enum VkSamplerAddressMode { VK_SAMPLER_ADDRESS_MODE_REPEAT = 0, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE = 2, TRIB_VK_ENUM_END(VK_SAMPLER_ADDRESS_MODE) };
enum VkSamplerMipmapMode { VK_SAMPLER_MIPMAP_MODE_NEAREST = 0, VK_SAMPLER_MIPMAP_MODE_LINEAR = 1, TRIB_VK_ENUM_END(VK_SAMPLER_MIPMAP_MODE) };
enum VkBorderColor { VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK = 0, TRIB_VK_ENUM_END(VK_BORDER_COLOR) };
enum VkSharingMode { VK_SHARING_MODE_EXCLUSIVE = 0, VK_SHARING_MODE_CONCURRENT = 1, TRIB_VK_ENUM_END(VK_SHARING_MODE) };
enum VkSubpassContents { VK_SUBPASS_CONTENTS_INLINE = 0, TRIB_VK_ENUM_END(VK_SUBPASS_CONTENTS) };
enum VkVertexInputRate { VK_VERTEX_INPUT_RATE_VERTEX = 0, VK_VERTEX_INPUT_RATE_INSTANCE = 1, TRIB_VK_ENUM_END(VK_VERTEX_INPUT_RATE) };
enum VkSurfaceTransformFlagBitsKHR { VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR = 1, TRIB_VK_ENUM_END(VK_SURFACE_TRANSFORM) };
enum VkCompositeAlphaFlagBitsKHR { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 1, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR = 2,
                                   VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR = 4, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR = 8,
                                   TRIB_VK_ENUM_END(VK_COMPOSITE_ALPHA) };
enum VkSampleCountFlagBits { VK_SAMPLE_COUNT_1_BIT = 1, TRIB_VK_ENUM_END(VK_SAMPLE_COUNT) };
enum VkShaderStageFlagBits { VK_SHADER_STAGE_VERTEX_BIT = 0x1, VK_SHADER_STAGE_FRAGMENT_BIT = 0x10, TRIB_VK_ENUM_END(VK_SHADER_STAGE) };
#undef TRIB_VK_ENUM_END

// flag bits (plain constants; the API takes them as VkFlags)
enum : VkFlags {
    VK_ACCESS_SHADER_READ_BIT = 0x20, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 0x100, VK_ACCESS_TRANSFER_READ_BIT = 0x800,
    VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x1, VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x80,
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1,
    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2,
    VK_COLOR_COMPONENT_R_BIT = 0x1, VK_COLOR_COMPONENT_G_BIT = 0x2, VK_COLOR_COMPONENT_B_BIT = 0x4, VK_COLOR_COMPONENT_A_BIT = 0x8,
    VK_CULL_MODE_NONE = 0,
    VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT = 0x1,
    VK_FENCE_CREATE_SIGNALED_BIT = 0x1,
    VK_IMAGE_ASPECT_COLOR_BIT = 0x1,
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x1, VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2, VK_IMAGE_USAGE_SAMPLED_BIT = 0x4,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10,
    VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR = 0x1,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x1, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x2, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x4,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x1, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 0x80,
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x400, VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000,
    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 0x2000,
    VK_QUEUE_GRAPHICS_BIT = 0x1,
};

typedef VkFlags VkInstanceCreateFlags, VkDeviceCreateFlags, VkDeviceQueueCreateFlags, VkQueueFlags, VkMemoryPropertyFlags,
    VkMemoryHeapFlags, VkBufferCreateFlags, VkBufferUsageFlags, VkImageCreateFlags, VkImageUsageFlags, VkImageViewCreateFlags,
    VkImageAspectFlags, VkSampleCountFlags, VkShaderModuleCreateFlags, VkPipelineCreateFlags, VkPipelineShaderStageCreateFlags,
    VkPipelineVertexInputStateCreateFlags, VkPipelineInputAssemblyStateCreateFlags, VkPipelineTessellationStateCreateFlags,
    VkPipelineViewportStateCreateFlags, VkPipelineRasterizationStateCreateFlags, VkCullModeFlags,
    VkPipelineMultisampleStateCreateFlags, VkPipelineDepthStencilStateCreateFlags, VkPipelineColorBlendStateCreateFlags,
    VkColorComponentFlags, VkPipelineDynamicStateCreateFlags, VkPipelineLayoutCreateFlags, VkShaderStageFlags,
    VkSamplerCreateFlags, VkDescriptorSetLayoutCreateFlags, VkDescriptorPoolCreateFlags, VkDescriptorPoolResetFlags,
    VkFramebufferCreateFlags, VkRenderPassCreateFlags, VkAttachmentDescriptionFlags, VkSubpassDescriptionFlags,
    VkAccessFlags, VkDependencyFlags, VkCommandPoolCreateFlags, VkCommandBufferUsageFlags, VkCommandBufferResetFlags,
    VkPipelineStageFlags, VkFenceCreateFlags, VkSemaphoreCreateFlags, VkMemoryMapFlags, VkSurfaceTransformFlagsKHR,
    VkCompositeAlphaFlagsKHR, VkSwapchainCreateFlagsKHR, VkXlibSurfaceCreateFlagsKHR, VkWin32SurfaceCreateFlagsKHR;

struct VkAllocationCallbacks;
struct VkPhysicalDeviceFeatures;
struct VkSpecializationInfo;
struct VkPipelineTessellationStateCreateInfo;
struct VkPipelineDepthStencilStateCreateInfo;
struct VkCommandBufferInheritanceInfo;
struct VkDescriptorBufferInfo;

struct VkOffset2D { int32_t x, y; };
struct VkOffset3D { int32_t x, y, z; };
struct VkExtent2D { uint32_t width, height; };
struct VkExtent3D { uint32_t width, height, depth; };
struct VkRect2D { VkOffset2D offset; VkExtent2D extent; };
struct VkViewport { float x, y, width, height, minDepth, maxDepth; };

struct VkApplicationInfo {
    VkStructureType sType; const void* pNext;
    const char* pApplicationName; uint32_t applicationVersion;
    const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion;
};
struct VkInstanceCreateInfo {
    VkStructureType sType; const void* pNext; VkInstanceCreateFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
};
struct VkExtensionProperties { char extensionName[256]; uint32_t specVersion; };
struct VkQueueFamilyProperties { VkQueueFlags queueFlags; uint32_t queueCount; uint32_t timestampValidBits; VkExtent3D minImageTransferGranularity; };
// The driver fills in VkPhysicalDeviceLimits and sparse properties after the fields below; we
// don't read them, so they're reserved space, sized generously.
struct VkPhysicalDeviceProperties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    VkPhysicalDeviceType deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    alignas(8) uint8_t reserved_limits_and_sparse[2048];
};
struct VkMemoryType { VkMemoryPropertyFlags propertyFlags; uint32_t heapIndex; };
struct VkMemoryHeap { VkDeviceSize size; VkMemoryHeapFlags flags; };
struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
};
struct VkDeviceQueueCreateInfo {
    VkStructureType sType; const void* pNext; VkDeviceQueueCreateFlags flags;
    uint32_t queueFamilyIndex; uint32_t queueCount; const float* pQueuePriorities;
};
struct VkDeviceCreateInfo {
    VkStructureType sType; const void* pNext; VkDeviceCreateFlags flags;
    uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
    const VkPhysicalDeviceFeatures* pEnabledFeatures;
};
struct VkSurfaceCapabilitiesKHR {
    uint32_t minImageCount, maxImageCount;
    VkExtent2D currentExtent, minImageExtent, maxImageExtent;
    uint32_t maxImageArrayLayers;
    VkSurfaceTransformFlagsKHR supportedTransforms;
    VkSurfaceTransformFlagBitsKHR currentTransform;
    VkCompositeAlphaFlagsKHR supportedCompositeAlpha;
    VkImageUsageFlags supportedUsageFlags;
};
struct VkSurfaceFormatKHR { VkFormat format; VkColorSpaceKHR colorSpace; };
struct VkSwapchainCreateInfoKHR {
    VkStructureType sType; const void* pNext; VkSwapchainCreateFlagsKHR flags;
    VkSurfaceKHR surface; uint32_t minImageCount; VkFormat imageFormat; VkColorSpaceKHR imageColorSpace;
    VkExtent2D imageExtent; uint32_t imageArrayLayers; VkImageUsageFlags imageUsage; VkSharingMode imageSharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
    VkSurfaceTransformFlagBitsKHR preTransform; VkCompositeAlphaFlagBitsKHR compositeAlpha; VkPresentModeKHR presentMode;
    VkBool32 clipped; VkSwapchainKHR oldSwapchain;
};
struct VkComponentMapping { VkComponentSwizzle r, g, b, a; };
struct VkImageSubresourceRange { VkImageAspectFlags aspectMask; uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount; };
struct VkImageSubresourceLayers { VkImageAspectFlags aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount; };
struct VkImageViewCreateInfo {
    VkStructureType sType; const void* pNext; VkImageViewCreateFlags flags;
    VkImage image; VkImageViewType viewType; VkFormat format; VkComponentMapping components; VkImageSubresourceRange subresourceRange;
};
struct VkAttachmentDescription {
    VkAttachmentDescriptionFlags flags; VkFormat format; VkSampleCountFlagBits samples;
    VkAttachmentLoadOp loadOp; VkAttachmentStoreOp storeOp; VkAttachmentLoadOp stencilLoadOp; VkAttachmentStoreOp stencilStoreOp;
    VkImageLayout initialLayout, finalLayout;
};
struct VkAttachmentReference { uint32_t attachment; VkImageLayout layout; };
struct VkSubpassDescription {
    VkSubpassDescriptionFlags flags; VkPipelineBindPoint pipelineBindPoint;
    uint32_t inputAttachmentCount; const VkAttachmentReference* pInputAttachments;
    uint32_t colorAttachmentCount; const VkAttachmentReference* pColorAttachments; const VkAttachmentReference* pResolveAttachments;
    const VkAttachmentReference* pDepthStencilAttachment;
    uint32_t preserveAttachmentCount; const uint32_t* pPreserveAttachments;
};
struct VkSubpassDependency {
    uint32_t srcSubpass, dstSubpass; VkPipelineStageFlags srcStageMask, dstStageMask;
    VkAccessFlags srcAccessMask, dstAccessMask; VkDependencyFlags dependencyFlags;
};
struct VkRenderPassCreateInfo {
    VkStructureType sType; const void* pNext; VkRenderPassCreateFlags flags;
    uint32_t attachmentCount; const VkAttachmentDescription* pAttachments;
    uint32_t subpassCount; const VkSubpassDescription* pSubpasses;
    uint32_t dependencyCount; const VkSubpassDependency* pDependencies;
};
struct VkFramebufferCreateInfo {
    VkStructureType sType; const void* pNext; VkFramebufferCreateFlags flags;
    VkRenderPass renderPass; uint32_t attachmentCount; const VkImageView* pAttachments; uint32_t width, height, layers;
};
struct VkShaderModuleCreateInfo { VkStructureType sType; const void* pNext; VkShaderModuleCreateFlags flags; size_t codeSize; const uint32_t* pCode; };
struct VkPipelineShaderStageCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineShaderStageCreateFlags flags;
    VkShaderStageFlagBits stage; VkShaderModule module; const char* pName; const VkSpecializationInfo* pSpecializationInfo;
};
struct VkVertexInputBindingDescription { uint32_t binding, stride; VkVertexInputRate inputRate; };
struct VkVertexInputAttributeDescription { uint32_t location, binding; VkFormat format; uint32_t offset; };
struct VkPipelineVertexInputStateCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineVertexInputStateCreateFlags flags;
    uint32_t vertexBindingDescriptionCount; const VkVertexInputBindingDescription* pVertexBindingDescriptions;
    uint32_t vertexAttributeDescriptionCount; const VkVertexInputAttributeDescription* pVertexAttributeDescriptions;
};
struct VkPipelineInputAssemblyStateCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineInputAssemblyStateCreateFlags flags;
    VkPrimitiveTopology topology; VkBool32 primitiveRestartEnable;
};
struct VkPipelineViewportStateCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineViewportStateCreateFlags flags;
    uint32_t viewportCount; const VkViewport* pViewports; uint32_t scissorCount; const VkRect2D* pScissors;
};
struct VkPipelineRasterizationStateCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineRasterizationStateCreateFlags flags;
    VkBool32 depthClampEnable, rasterizerDiscardEnable; VkPolygonMode polygonMode; VkCullModeFlags cullMode; VkFrontFace frontFace;
    VkBool32 depthBiasEnable; float depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor, lineWidth;
};
struct VkPipelineMultisampleStateCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineMultisampleStateCreateFlags flags;
    VkSampleCountFlagBits rasterizationSamples; VkBool32 sampleShadingEnable; float minSampleShading;
    const VkSampleMask* pSampleMask; VkBool32 alphaToCoverageEnable, alphaToOneEnable;
};
struct VkPipelineColorBlendAttachmentState {
    VkBool32 blendEnable; VkBlendFactor srcColorBlendFactor, dstColorBlendFactor; VkBlendOp colorBlendOp;
    VkBlendFactor srcAlphaBlendFactor, dstAlphaBlendFactor; VkBlendOp alphaBlendOp; VkColorComponentFlags colorWriteMask;
};
struct VkPipelineColorBlendStateCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineColorBlendStateCreateFlags flags;
    VkBool32 logicOpEnable; VkLogicOp logicOp; uint32_t attachmentCount; const VkPipelineColorBlendAttachmentState* pAttachments;
    float blendConstants[4];
};
struct VkPipelineDynamicStateCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineDynamicStateCreateFlags flags;
    uint32_t dynamicStateCount; const VkDynamicState* pDynamicStates;
};
struct VkGraphicsPipelineCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineCreateFlags flags;
    uint32_t stageCount; const VkPipelineShaderStageCreateInfo* pStages;
    const VkPipelineVertexInputStateCreateInfo* pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
    const VkPipelineTessellationStateCreateInfo* pTessellationState;
    const VkPipelineViewportStateCreateInfo* pViewportState;
    const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo* pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfo* pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfo* pColorBlendState;
    const VkPipelineDynamicStateCreateInfo* pDynamicState;
    VkPipelineLayout layout; VkRenderPass renderPass; uint32_t subpass; VkPipeline basePipelineHandle; int32_t basePipelineIndex;
};
struct VkPushConstantRange { VkShaderStageFlags stageFlags; uint32_t offset, size; };
struct VkPipelineLayoutCreateInfo {
    VkStructureType sType; const void* pNext; VkPipelineLayoutCreateFlags flags;
    uint32_t setLayoutCount; const VkDescriptorSetLayout* pSetLayouts;
    uint32_t pushConstantRangeCount; const VkPushConstantRange* pPushConstantRanges;
};
struct VkDescriptorSetLayoutBinding {
    uint32_t binding; VkDescriptorType descriptorType; uint32_t descriptorCount; VkShaderStageFlags stageFlags;
    const VkSampler* pImmutableSamplers;
};
struct VkDescriptorSetLayoutCreateInfo {
    VkStructureType sType; const void* pNext; VkDescriptorSetLayoutCreateFlags flags;
    uint32_t bindingCount; const VkDescriptorSetLayoutBinding* pBindings;
};
struct VkDescriptorPoolSize { VkDescriptorType type; uint32_t descriptorCount; };
struct VkDescriptorPoolCreateInfo {
    VkStructureType sType; const void* pNext; VkDescriptorPoolCreateFlags flags;
    uint32_t maxSets; uint32_t poolSizeCount; const VkDescriptorPoolSize* pPoolSizes;
};
struct VkDescriptorSetAllocateInfo {
    VkStructureType sType; const void* pNext; VkDescriptorPool descriptorPool;
    uint32_t descriptorSetCount; const VkDescriptorSetLayout* pSetLayouts;
};
struct VkDescriptorImageInfo { VkSampler sampler; VkImageView imageView; VkImageLayout imageLayout; };
struct VkWriteDescriptorSet {
    VkStructureType sType; const void* pNext; VkDescriptorSet dstSet; uint32_t dstBinding, dstArrayElement, descriptorCount;
    VkDescriptorType descriptorType; const VkDescriptorImageInfo* pImageInfo; const VkDescriptorBufferInfo* pBufferInfo;
    const VkBufferView* pTexelBufferView;
};
struct VkCopyDescriptorSet;
struct VkSamplerCreateInfo {
    VkStructureType sType; const void* pNext; VkSamplerCreateFlags flags;
    VkFilter magFilter, minFilter; VkSamplerMipmapMode mipmapMode;
    VkSamplerAddressMode addressModeU, addressModeV, addressModeW;
    float mipLodBias; VkBool32 anisotropyEnable; float maxAnisotropy; VkBool32 compareEnable; VkCompareOp compareOp;
    float minLod, maxLod; VkBorderColor borderColor; VkBool32 unnormalizedCoordinates;
};
struct VkCommandPoolCreateInfo { VkStructureType sType; const void* pNext; VkCommandPoolCreateFlags flags; uint32_t queueFamilyIndex; };
struct VkCommandBufferAllocateInfo {
    VkStructureType sType; const void* pNext; VkCommandPool commandPool; VkCommandBufferLevel level; uint32_t commandBufferCount;
};
struct VkCommandBufferBeginInfo {
    VkStructureType sType; const void* pNext; VkCommandBufferUsageFlags flags; const VkCommandBufferInheritanceInfo* pInheritanceInfo;
};
union VkClearColorValue { float float32[4]; int32_t int32[4]; uint32_t uint32[4]; };
struct VkClearDepthStencilValue { float depth; uint32_t stencil; };
union VkClearValue { VkClearColorValue color; VkClearDepthStencilValue depthStencil; };
struct VkRenderPassBeginInfo {
    VkStructureType sType; const void* pNext; VkRenderPass renderPass; VkFramebuffer framebuffer; VkRect2D renderArea;
    uint32_t clearValueCount; const VkClearValue* pClearValues;
};
struct VkBufferImageCopy {
    VkDeviceSize bufferOffset; uint32_t bufferRowLength, bufferImageHeight;
    VkImageSubresourceLayers imageSubresource; VkOffset3D imageOffset; VkExtent3D imageExtent;
};
struct VkMemoryBarrier;
struct VkBufferMemoryBarrier;
struct VkImageMemoryBarrier {
    VkStructureType sType; const void* pNext; VkAccessFlags srcAccessMask, dstAccessMask;
    VkImageLayout oldLayout, newLayout; uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex;
    VkImage image; VkImageSubresourceRange subresourceRange;
};
struct VkSemaphoreCreateInfo { VkStructureType sType; const void* pNext; VkSemaphoreCreateFlags flags; };
struct VkFenceCreateInfo { VkStructureType sType; const void* pNext; VkFenceCreateFlags flags; };
struct VkSubmitInfo {
    VkStructureType sType; const void* pNext;
    uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores; const VkPipelineStageFlags* pWaitDstStageMask;
    uint32_t commandBufferCount; const VkCommandBuffer* pCommandBuffers;
    uint32_t signalSemaphoreCount; const VkSemaphore* pSignalSemaphores;
};
struct VkPresentInfoKHR {
    VkStructureType sType; const void* pNext;
    uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores;
    uint32_t swapchainCount; const VkSwapchainKHR* pSwapchains; const uint32_t* pImageIndices; VkResult* pResults;
};
struct VkBufferCreateInfo {
    VkStructureType sType; const void* pNext; VkBufferCreateFlags flags; VkDeviceSize size; VkBufferUsageFlags usage;
    VkSharingMode sharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
};
struct VkMemoryRequirements { VkDeviceSize size, alignment; uint32_t memoryTypeBits; };
struct VkMemoryAllocateInfo { VkStructureType sType; const void* pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; };
struct VkImageCreateInfo {
    VkStructureType sType; const void* pNext; VkImageCreateFlags flags; VkImageType imageType; VkFormat format;
    VkExtent3D extent; uint32_t mipLevels, arrayLayers; VkSampleCountFlagBits samples; VkImageTiling tiling;
    VkImageUsageFlags usage; VkSharingMode sharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
    VkImageLayout initialLayout;
};

// ---------------------------------------------------------------- entry points
typedef void(TRIB_VKAPI* PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction(TRIB_VKAPI* PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction(TRIB_VKAPI* PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
typedef VkResult(TRIB_VKAPI* PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, VkExtensionProperties*);
typedef void(TRIB_VKAPI* PFN_vkDestroyInstance)(VkInstance, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void(TRIB_VKAPI* PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
typedef void(TRIB_VKAPI* PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
typedef void(TRIB_VKAPI* PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
typedef VkResult(TRIB_VKAPI* PFN_vkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
typedef VkResult(TRIB_VKAPI* PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
typedef VkResult(TRIB_VKAPI* PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
typedef VkResult(TRIB_VKAPI* PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
typedef void(TRIB_VKAPI* PFN_vkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkEnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
typedef void(TRIB_VKAPI* PFN_vkDestroyDevice)(VkDevice, const VkAllocationCallbacks*);
typedef void(TRIB_VKAPI* PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
typedef void(TRIB_VKAPI* PFN_vkDestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkGetSwapchainImagesKHR)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
typedef VkResult(TRIB_VKAPI* PFN_vkAcquireNextImageKHR)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
typedef VkResult(TRIB_VKAPI* PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateImageView)(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*);
typedef void(TRIB_VKAPI* PFN_vkDestroyImageView)(VkDevice, VkImageView, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateRenderPass)(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
typedef void(TRIB_VKAPI* PFN_vkDestroyRenderPass)(VkDevice, VkRenderPass, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateFramebuffer)(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
typedef void(TRIB_VKAPI* PFN_vkDestroyFramebuffer)(VkDevice, VkFramebuffer, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
typedef void(TRIB_VKAPI* PFN_vkDestroyShaderModule)(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*);
typedef void(TRIB_VKAPI* PFN_vkDestroyPipelineLayout)(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateGraphicsPipelines)(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*,
                                                            const VkAllocationCallbacks*, VkPipeline*);
typedef void(TRIB_VKAPI* PFN_vkDestroyPipeline)(VkDevice, VkPipeline, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*);
typedef void(TRIB_VKAPI* PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef void(TRIB_VKAPI* PFN_vkFreeCommandBuffers)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
typedef VkResult(TRIB_VKAPI* PFN_vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
typedef VkResult(TRIB_VKAPI* PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef VkResult(TRIB_VKAPI* PFN_vkResetCommandBuffer)(VkCommandBuffer, VkCommandBufferResetFlags);
typedef void(TRIB_VKAPI* PFN_vkCmdBeginRenderPass)(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
typedef void(TRIB_VKAPI* PFN_vkCmdEndRenderPass)(VkCommandBuffer);
typedef void(TRIB_VKAPI* PFN_vkCmdBindPipeline)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
typedef void(TRIB_VKAPI* PFN_vkCmdBindVertexBuffers)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*);
typedef void(TRIB_VKAPI* PFN_vkCmdDraw)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void(TRIB_VKAPI* PFN_vkCmdSetViewport)(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
typedef void(TRIB_VKAPI* PFN_vkCmdSetScissor)(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
typedef void(TRIB_VKAPI* PFN_vkCmdPushConstants)(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*);
typedef void(TRIB_VKAPI* PFN_vkCmdBindDescriptorSets)(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                                                      const VkDescriptorSet*, uint32_t, const uint32_t*);
typedef void(TRIB_VKAPI* PFN_vkCmdCopyBufferToImage)(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*);
typedef void(TRIB_VKAPI* PFN_vkCmdCopyImageToBuffer)(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*);
typedef void(TRIB_VKAPI* PFN_vkCmdPipelineBarrier)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                                   uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                                                   const VkImageMemoryBarrier*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateSemaphore)(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*);
typedef void(TRIB_VKAPI* PFN_vkDestroySemaphore)(VkDevice, VkSemaphore, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateFence)(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence*);
typedef void(TRIB_VKAPI* PFN_vkDestroyFence)(VkDevice, VkFence, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkWaitForFences)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
typedef VkResult(TRIB_VKAPI* PFN_vkResetFences)(VkDevice, uint32_t, const VkFence*);
typedef VkResult(TRIB_VKAPI* PFN_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
typedef VkResult(TRIB_VKAPI* PFN_vkQueueWaitIdle)(VkQueue);
typedef VkResult(TRIB_VKAPI* PFN_vkDeviceWaitIdle)(VkDevice);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateBuffer)(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
typedef void(TRIB_VKAPI* PFN_vkDestroyBuffer)(VkDevice, VkBuffer, const VkAllocationCallbacks*);
typedef void(TRIB_VKAPI* PFN_vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements*);
typedef VkResult(TRIB_VKAPI* PFN_vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
typedef void(TRIB_VKAPI* PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef VkResult(TRIB_VKAPI* PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void**);
typedef void(TRIB_VKAPI* PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateImage)(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
typedef void(TRIB_VKAPI* PFN_vkDestroyImage)(VkDevice, VkImage, const VkAllocationCallbacks*);
typedef void(TRIB_VKAPI* PFN_vkGetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements*);
typedef VkResult(TRIB_VKAPI* PFN_vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateSampler)(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler*);
typedef void(TRIB_VKAPI* PFN_vkDestroySampler)(VkDevice, VkSampler, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*,
                                                              VkDescriptorSetLayout*);
typedef void(TRIB_VKAPI* PFN_vkDestroyDescriptorSetLayout)(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkCreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool*);
typedef void(TRIB_VKAPI* PFN_vkDestroyDescriptorPool)(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*);
typedef VkResult(TRIB_VKAPI* PFN_vkAllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
typedef VkResult(TRIB_VKAPI* PFN_vkFreeDescriptorSets)(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet*);
typedef void(TRIB_VKAPI* PFN_vkUpdateDescriptorSets)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);

// ---------------------------------------------------------------- window-system surfaces
#if defined(_WIN32)
struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType sType; const void* pNext; VkWin32SurfaceCreateFlagsKHR flags; struct HINSTANCE__* hinstance; struct HWND__* hwnd;
};
typedef VkResult(TRIB_VKAPI* PFN_vkCreateWin32SurfaceKHR)(VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
#elif !defined(__APPLE__)
struct VkXlibSurfaceCreateInfoKHR {
    VkStructureType sType; const void* pNext; VkXlibSurfaceCreateFlagsKHR flags; struct _XDisplay* dpy; unsigned long window;
};
typedef VkResult(TRIB_VKAPI* PFN_vkCreateXlibSurfaceKHR)(VkInstance, const VkXlibSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
#endif

#ifdef TRIBUTARY_VK_NAMESPACE
}  // namespace TRIBUTARY_VK_NAMESPACE
#endif
