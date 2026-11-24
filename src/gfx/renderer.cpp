// SPDX-License-Identifier: MIT
#include "renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

#include "embedded.hpp"
#include "renderer_batch.hpp"

namespace gfx {

Rect Rect::intersect(const Rect& o) const {
    float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    float x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
    return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
}

#ifndef __APPLE__
// ================================================================ Vulkan function table
#define VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE_FNS(X)                                                                                  \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)                      \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                    \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X)                                                                                         \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)                       \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateImageView)               \
    X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer)                   \
    X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout)          \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateCommandPool)         \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)           \
    X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                  \
    X(vkCmdBindVertexBuffers) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPushConstants)         \
    X(vkCmdBindDescriptorSets) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier)      \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)            \
    X(vkResetFences) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkCreateBuffer) X(vkDestroyBuffer)                  \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory)  \
    X(vkUnmapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)    \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)       \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)  \
    X(vkQueueWaitIdle) X(vkFreeDescriptorSets) X(vkFreeCommandBuffers)

#define DECL(f) PFN_##f f = nullptr;
struct VkFns {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    VK_GLOBAL_FNS(DECL)
    VK_INSTANCE_FNS(DECL)
    VK_DEVICE_FNS(DECL)
};
#undef DECL

static void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")");
}

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};



struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t last_used = 0;
};

struct Renderer::Impl {
    VkFns vk;
    plat::Window* win;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memprops{};

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;

    static constexpr int FRAMES = 2;
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        Buffer instances;
    } frames[FRAMES];
    int frame_index = 0;
    uint32_t image_index = 0;
    bool swapchain_ok = false;
    bool supports_capture = false;

    VkImage atlas = VK_NULL_HANDLE;
    VkDeviceMemory atlas_mem = VK_NULL_HANDLE;
    VkImageView atlas_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    Buffer staging;
    bool atlas_initialized = false;

    Buffer readback;
    bool capture_pending = false, capture_ready = false;
    int capture_w = 0, capture_h = 0;

    std::vector<Batch> batches;
    Color clear{};
    std::map<uint64_t, Texture> textures;
    Texture dummy;
    VkSampler linear_sampler = VK_NULL_HANDLE;
    uint64_t frame_counter = 0;

    VkCommandBuffer begin_once() {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        vk.vkAllocateCommandBuffers(device, &ai, &cb);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(cb, &bi);
        return cb;
    }
    void end_once(VkCommandBuffer cb) {
        vk.vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vk.vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vk.vkQueueWaitIdle(queue);
        vk.vkFreeCommandBuffers(device, cmd_pool, 1, &cb);
    }

    Texture make_texture(int w, int h, const uint8_t* rgba) {
        Texture t;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {uint32_t(w), uint32_t(h), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vk.vkCreateImage(device, &ii, nullptr, &t.image), "vkCreateImage");
        VkMemoryRequirements req;
        vk.vkGetImageMemoryRequirements(device, t.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vk.vkAllocateMemory(device, &ai, nullptr, &t.mem), "vkAllocateMemory");
        vk.vkBindImageMemory(device, t.image, t.mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vk.vkCreateImageView(device, &vi, nullptr, &t.view), "vkCreateImageView");
        // upload
        Buffer stage = make_buffer(VkDeviceSize(w) * VkDeviceSize(h) * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(stage.mapped, rgba, size_t(w) * size_t(h) * 4);
        VkCommandBuffer cb = begin_once();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {uint32_t(w), uint32_t(h), 1};
        vk.vkCmdCopyBufferToImage(cb, stage.buf, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        end_once(cb);
        free_buffer(stage);
        // descriptor set: binding 0 = glyph atlas, binding 1 = this image
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &dsl;
        check(vk.vkAllocateDescriptorSets(device, &dai, &t.set), "vkAllocateDescriptorSets");
        write_set(t.set, t.view);
        return t;
    }

    void write_set(VkDescriptorSet set, VkImageView image_view) {
        VkDescriptorImageInfo infos[2] = {{sampler, atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                          {linear_sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[2]{};
        for (int i = 0; i < 2; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = uint32_t(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &infos[i];
        }
        vk.vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
    }

    void destroy_texture(Texture& t) {
        if (t.set) vk.vkFreeDescriptorSets(device, dpool, 1, &t.set);
        if (t.view) vk.vkDestroyImageView(device, t.view, nullptr);
        if (t.image) vk.vkDestroyImage(device, t.image, nullptr);
        if (t.mem) vk.vkFreeMemory(device, t.mem, nullptr);
        t = {};
    }

    uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no suitable Vulkan memory type");
    }

    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b;
        b.size = size;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vk.vkCreateBuffer(device, &ci, nullptr, &b.buf), "vkCreateBuffer");
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(device, b.buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vk.vkAllocateMemory(device, &ai, nullptr, &b.mem), "vkAllocateMemory");
        vk.vkBindBufferMemory(device, b.buf, b.mem, 0);
        vk.vkMapMemory(device, b.mem, 0, size, 0, &b.mapped);
        return b;
    }

    void free_buffer(Buffer& b) {
        if (!b.buf) return;
        vk.vkUnmapMemory(device, b.mem);
        vk.vkDestroyBuffer(device, b.buf, nullptr);
        vk.vkFreeMemory(device, b.mem, nullptr);
        b = {};
    }

    void init(plat::Window& w) {
        win = &w;
        vk.vkGetInstanceProcAddr = plat::load_vulkan();
        if (!vk.vkGetInstanceProcAddr)
            throw std::runtime_error("Vulkan is not available. Install your GPU driver's Vulkan support"
#ifdef __APPLE__
                                     " (the app bundles MoltenVK; reinstall the app if it's missing)"
#endif
                                     ".");
#define LOADG(f) vk.f = reinterpret_cast<PFN_##f>(vk.vkGetInstanceProcAddr(nullptr, #f));
        VK_GLOBAL_FNS(LOADG)
#undef LOADG
        auto exts = w.vk_instance_extensions();
        uint32_t n = 0;
        vk.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        vk.vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());
        bool portability = false;
        for (auto& e : avail)
            if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) portability = true;
        if (portability) exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Tributary";
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = uint32_t(exts.size());
        ici.ppEnabledExtensionNames = exts.data();
        if (portability) ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        check(vk.vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
#define LOADI(f) vk.f = reinterpret_cast<PFN_##f>(vk.vkGetInstanceProcAddr(instance, #f));
        VK_INSTANCE_FNS(LOADI)
#undef LOADI
        surface = w.vk_create_surface(instance, vk.vkGetInstanceProcAddr);

        uint32_t ndev = 0;
        vk.vkEnumeratePhysicalDevices(instance, &ndev, nullptr);
        std::vector<VkPhysicalDevice> devs(ndev);
        vk.vkEnumeratePhysicalDevices(instance, &ndev, devs.data());
        int best_score = -1;
        for (auto d : devs) {
            VkPhysicalDeviceProperties props;
            vk.vkGetPhysicalDeviceProperties(d, &props);
            uint32_t nq = 0;
            vk.vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
            std::vector<VkQueueFamilyProperties> qs(nq);
            vk.vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
            for (uint32_t i = 0; i < nq; i++) {
                VkBool32 present = VK_FALSE;
                vk.vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
                if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                // integrated GPUs are fine (and save power) for a 2D UI
                int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                            : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                            : 1;
                if (score > best_score) { best_score = score; phys = d; qfam = i; }
                break;
            }
        }
        if (!phys) throw std::runtime_error("no Vulkan GPU can present to this window");
        vk.vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

        float prio = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        uint32_t nde = 0;
        vk.vkEnumerateDeviceExtensionProperties(phys, nullptr, &nde, nullptr);
        std::vector<VkExtensionProperties> davail(nde);
        vk.vkEnumerateDeviceExtensionProperties(phys, nullptr, &nde, davail.data());
        for (auto& e : davail)
            if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) dexts.push_back("VK_KHR_portability_subset");
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(dexts.size());
        dci.ppEnabledExtensionNames = dexts.data();
        check(vk.vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice");
#define LOADD(f) vk.f = reinterpret_cast<PFN_##f>(vk.vkGetDeviceProcAddr(device, #f));
        VK_DEVICE_FNS(LOADD)
#undef LOADD
        vk.vkGetDeviceQueue(device, qfam, 0, &queue);

        // pick a surface format
        uint32_t nf = 0;
        vk.vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(nf);
        vk.vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, fmts.data());
        format = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0].format;
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { format = f.format; break; }

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = qfam;
        check(vk.vkCreateCommandPool(device, &cpi, nullptr, &cmd_pool), "vkCreateCommandPool");
        for (auto& f : frames) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = cmd_pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            check(vk.vkAllocateCommandBuffers(device, &ai, &f.cmd), "vkAllocateCommandBuffers");
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vk.vkCreateSemaphore(device, &si, nullptr, &f.acquired);
            vk.vkCreateSemaphore(device, &si, nullptr, &f.rendered);
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vk.vkCreateFence(device, &fi, nullptr, &f.fence);
            f.instances = make_buffer(sizeof(Instance) * 16384, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        }
        create_render_pass();
        create_atlas();
        create_pipeline();
        create_swapchain();
    }

    void create_render_pass() {
        VkAttachmentDescription att{};
        att.format = format;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1;
        rpi.pAttachments = &att;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sub;
        rpi.dependencyCount = 1;
        rpi.pDependencies = &dep;
        check(vk.vkCreateRenderPass(device, &rpi, nullptr, &render_pass), "vkCreateRenderPass");
    }

    void create_atlas() {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8_UNORM;
        ii.extent = {uint32_t(Atlas::SIZE), uint32_t(Atlas::SIZE), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vk.vkCreateImage(device, &ii, nullptr, &atlas), "vkCreateImage");
        VkMemoryRequirements req;
        vk.vkGetImageMemoryRequirements(device, atlas, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vk.vkAllocateMemory(device, &ai, nullptr, &atlas_mem), "vkAllocateMemory");
        vk.vkBindImageMemory(device, atlas, atlas_mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = atlas;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vk.vkCreateImageView(device, &vi, nullptr, &atlas_view), "vkCreateImageView");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vk.vkCreateSampler(device, &si, nullptr, &sampler), "vkCreateSampler");
        staging = make_buffer(VkDeviceSize(Atlas::SIZE) * Atlas::SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

        VkDescriptorSetLayoutBinding b[2]{};
        for (uint32_t i = 0; i < 2; i++) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 2;
        dli.pBindings = b;
        check(vk.vkCreateDescriptorSetLayout(device, &dli, nullptr, &dsl), "vkCreateDescriptorSetLayout");
        VkSamplerCreateInfo lsi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        lsi.magFilter = VK_FILTER_LINEAR;
        lsi.minFilter = VK_FILTER_LINEAR;
        lsi.addressModeU = lsi.addressModeV = lsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vk.vkCreateSampler(device, &lsi, nullptr, &linear_sampler), "vkCreateSampler");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpi.maxSets = 512;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        check(vk.vkCreateDescriptorPool(device, &dpi, nullptr, &dpool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &dsl;
        check(vk.vkAllocateDescriptorSets(device, &dai, &dset), "vkAllocateDescriptorSets");
        uint8_t white[4] = {255, 255, 255, 255};
        dummy = make_texture(1, 1, white);
        write_set(dset, dummy.view);
    }

    VkShaderModule shader(const char* name) {
        auto blob = embedded::get(name);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = blob.size;
        std::vector<uint32_t> code((blob.size + 3) / 4);
        std::memcpy(code.data(), blob.data, blob.size);
        ci.pCode = code.data();
        VkShaderModule m;
        check(vk.vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    }

    void create_pipeline() {
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        check(vk.vkCreatePipelineLayout(device, &pli, nullptr, &layout), "vkCreatePipelineLayout");
        VkShaderModule vs = shader("ui.vert.spv"), fs = shader("ui.frag.spv");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkVertexInputBindingDescription bind{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
        VkVertexInputAttributeDescription attrs[5];
        for (uint32_t i = 0; i < 5; i++) attrs[i] = {i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 5;
        vin.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vin;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pColorBlendState = &cb;
        gpi.pDynamicState = &ds;
        gpi.layout = layout;
        gpi.renderPass = render_pass;
        check(vk.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline), "vkCreateGraphicsPipelines");
        vk.vkDestroyShaderModule(device, vs, nullptr);
        vk.vkDestroyShaderModule(device, fs, nullptr);
    }

    void destroy_swapchain() {
        for (auto fb : framebuffers) vk.vkDestroyFramebuffer(device, fb, nullptr);
        for (auto v : views) vk.vkDestroyImageView(device, v, nullptr);
        framebuffers.clear();
        views.clear();
        images.clear();
    }

    void create_swapchain() {
        vk.vkDeviceWaitIdle(device);
        VkSurfaceCapabilitiesKHR caps;
        vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
        VkExtent2D ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFF) {
            ext.width = uint32_t(std::max(1, win->pixel_width()));
            ext.height = uint32_t(std::max(1, win->pixel_height()));
        }
        ext.width = std::clamp(ext.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        if (ext.width == 0 || ext.height == 0) { swapchain_ok = false; return; }
        uint32_t count = std::max(caps.minImageCount, 2u);
        if (caps.maxImageCount) count = std::min(count + 1, caps.maxImageCount);
        VkSwapchainKHR old = swapchain;
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = count;
        sci.imageFormat = format;
        sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sci.imageExtent = ext;
        sci.imageArrayLayers = 1;
        supports_capture = caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (supports_capture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                 ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                 : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1u));
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = old;
        check(vk.vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain), "vkCreateSwapchainKHR");
        destroy_swapchain();
        if (old) vk.vkDestroySwapchainKHR(device, old, nullptr);
        extent = ext;
        uint32_t n = 0;
        vk.vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
        images.resize(n);
        vk.vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
        for (auto img : images) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v;
            check(vk.vkCreateImageView(device, &vi, nullptr, &v), "vkCreateImageView");
            views.push_back(v);
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = render_pass;
            fi.attachmentCount = 1;
            fi.pAttachments = &v;
            fi.width = ext.width;
            fi.height = ext.height;
            fi.layers = 1;
            VkFramebuffer fb;
            check(vk.vkCreateFramebuffer(device, &fi, nullptr, &fb), "vkCreateFramebuffer");
            framebuffers.push_back(fb);
        }
        swapchain_ok = true;
    }

    void shutdown() {
        if (!device) return;
        vk.vkDeviceWaitIdle(device);
        destroy_swapchain();
        if (swapchain) vk.vkDestroySwapchainKHR(device, swapchain, nullptr);
        for (auto& f : frames) {
            free_buffer(f.instances);
            vk.vkDestroySemaphore(device, f.acquired, nullptr);
            vk.vkDestroySemaphore(device, f.rendered, nullptr);
            vk.vkDestroyFence(device, f.fence, nullptr);
        }
        for (auto& [k, t] : textures) destroy_texture(t);
        textures.clear();
        destroy_texture(dummy);
        vk.vkDestroySampler(device, linear_sampler, nullptr);
        free_buffer(staging);
        free_buffer(readback);
        vk.vkDestroyCommandPool(device, cmd_pool, nullptr);
        vk.vkDestroyPipeline(device, pipeline, nullptr);
        vk.vkDestroyPipelineLayout(device, layout, nullptr);
        vk.vkDestroyRenderPass(device, render_pass, nullptr);
        vk.vkDestroyDescriptorPool(device, dpool, nullptr);
        vk.vkDestroyDescriptorSetLayout(device, dsl, nullptr);
        vk.vkDestroySampler(device, sampler, nullptr);
        vk.vkDestroyImageView(device, atlas_view, nullptr);
        vk.vkDestroyImage(device, atlas, nullptr);
        vk.vkFreeMemory(device, atlas_mem, nullptr);
        vk.vkDestroyDevice(device, nullptr);
        vk.vkDestroySurfaceKHR(instance, surface, nullptr);
        vk.vkDestroyInstance(instance, nullptr);
        device = VK_NULL_HANDLE;
    }

    void upload_atlas(VkCommandBuffer cmd, Atlas& a) {
        if (a.dirty_y1 <= a.dirty_y0 && atlas_initialized) return;
        int y0 = atlas_initialized ? a.dirty_y0 : 0, y1 = atlas_initialized ? a.dirty_y1 : Atlas::SIZE;
        size_t off = size_t(y0) * Atlas::SIZE;
        std::memcpy(static_cast<uint8_t*>(staging.mapped) + off, a.px.data() + off, size_t(y1 - y0) * Atlas::SIZE);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = atlas_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = atlas;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.bufferOffset = off;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, y0, 0};
        region.imageExtent = {uint32_t(Atlas::SIZE), uint32_t(y1 - y0), 1};
        vk.vkCmdCopyBufferToImage(cmd, staging.buf, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                nullptr, 0, nullptr, 1, &b);
        atlas_initialized = true;
        a.dirty_y0 = Atlas::SIZE;
        a.dirty_y1 = 0;
    }
};

#endif  // !__APPLE__

// ================================================================ Renderer
#ifndef __APPLE__
Renderer::Renderer(plat::Window& win) : impl_(std::make_unique<Impl>()), win_(win) {
    impl_->init(win);
    scale_ = win.scale();
    text_.set_scale(scale_);
}

Renderer::~Renderer() { impl_->shutdown(); }

bool Renderer::begin_frame(Color clear) {
    auto& I = *impl_;
    scale_ = win_.scale() * zoom_;
    text_.set_scale(scale_);
    I.clear = clear;
    I.batches.clear();
    clips_.clear();
    layer_ = 0;
    if (win_.pixel_width() <= 0 || win_.pixel_height() <= 0) return false;
    I.frame_counter++;
    // evict GPU images not drawn for a while
    if (I.frame_counter % 120 == 0) {
        bool any = false;
        for (auto& [k, t] : I.textures) if (I.frame_counter - t.last_used > 600) any = true;
        if (any) {
            I.vk.vkDeviceWaitIdle(I.device);
            for (auto it = I.textures.begin(); it != I.textures.end();) {
                if (I.frame_counter - it->second.last_used > 600) { I.destroy_texture(it->second); it = I.textures.erase(it); }
                else ++it;
            }
        }
    }
    if (!I.swapchain_ok || I.extent.width != uint32_t(win_.pixel_width()) || I.extent.height != uint32_t(win_.pixel_height()))
        I.create_swapchain();
    return I.swapchain_ok;
}


void Renderer::push_instance_tex(const Instance& in, uint64_t tex) {
    auto& B = impl_->batches;
    Rect clip = current_clip();
    if (clip.w <= 0 || clip.h <= 0) return;
    // cull fully clipped quads (shadows extend by their blur)
    float pad = in.params[2] == 2.f ? in.params[3] * 2 : 0;
    Rect q{in.rect[0] / scale_ - pad, in.rect[1] / scale_ - pad, in.rect[2] / scale_ + 2 * pad, in.rect[3] / scale_ + 2 * pad};
    if (q.x > clip.right() || q.y > clip.bottom() || q.right() < clip.x || q.bottom() < clip.y) return;
    if (B.empty() || B.back().layer != layer_ || B.back().tex != tex || std::memcmp(&B.back().clip, &clip, sizeof(Rect)) != 0)
        B.push_back({layer_, clip, tex, {}});
    B.back().items.push_back(in);
}

#endif

void Renderer::push_instance(const Instance& in) { push_instance_tex(in, 0); }

void Renderer::rect(const Rect& r, Color c, float radius, float border, Color bc) {
    if (c.a <= 0 && (border <= 0 || bc.a <= 0)) return;
    float s = scale_;
    Instance in{{r.x * s, r.y * s, r.w * s, r.h * s}, {0, 0, 0, 0}, {c.r, c.g, c.b, c.a}, {bc.r, bc.g, bc.b, bc.a},
                {radius * s, border * s, 0, 0}};
    push_instance(in);
}

void Renderer::shadow(const Rect& r, float radius, float blur, Color c) {
    float s = scale_;
    Instance in{{r.x * s, r.y * s, r.w * s, r.h * s}, {0, 0, 0, 0}, {c.r, c.g, c.b, c.a}, {0, 0, 0, 0},
                {radius * s, 0, 2, blur * s}};
    push_instance(in);
}

void Renderer::line_h(float x0, float x1, float y, Color c, float t) { rect({x0, y, x1 - x0, t}, c); }
void Renderer::line_v(float x, float y0, float y1, Color c, float t) { rect({x, y0, t, y1 - y0}, c); }

float Renderer::draw_text(FontId f, float size, float x, float y, std::string_view str, Color c) {
    auto run = text_.shape(f, size, str);
    float s = scale_;
    float px_size = size * s;
    float base = std::round((y + text_.ascent(f, size)) * s);
    for (auto& g : run.glyphs) {
        float gx = (x + g.x) * s;
        float fx = std::floor(gx);
        int sub = int((gx - fx) * 4) & 3;
        auto& gi = text_.glyph(g.font, g.glyph, px_size, sub);
        if (!gi.valid) continue;
        Instance in{{fx + gi.off_x, base + gi.off_y, float(gi.aw), float(gi.ah)},
                    {float(gi.ax), float(gi.ay), float(gi.ax + gi.aw), float(gi.ay + gi.ah)},
                    {c.r, c.g, c.b, c.a}, {0, 0, 0, 0}, {0, 0, 1, 0}};
        push_instance(in);
    }
    return run.width;
}

std::string Renderer::ellipsize(FontId f, float size, std::string_view s, float max_w) {
    if (text_.measure(f, size, s) <= max_w) return std::string(s);
    const char* dots = "\xE2\x80\xA6";
    float dw = text_.measure(f, size, dots);
    size_t n = s.size();
    // binary search the longest prefix that fits
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        size_t b = mid;
        while (b > 0 && b < n && ((unsigned char)s[b] & 0xC0) == 0x80) b--;
        if (text_.measure(f, size, s.substr(0, b)) + dw <= max_w) lo = mid; else hi = mid - 1;
    }
    size_t b = lo;
    while (b > 0 && b < n && ((unsigned char)s[b] & 0xC0) == 0x80) b--;
    return std::string(s.substr(0, b)) + dots;
}

std::string Renderer::ellipsize_middle(FontId f, float size, std::string_view s, float max_w) {
    if (text_.measure(f, size, s) <= max_w) return std::string(s);
    const char* dots = "\xE2\x80\xA6";
    size_t keep_end = s.size() / 2;
    for (size_t cut = 1; cut < s.size(); cut++) {
        size_t a = (s.size() - cut) / 2, b = s.size() - (s.size() - cut - a);
        while (a > 0 && ((unsigned char)s[a] & 0xC0) == 0x80) a--;
        while (b < s.size() && ((unsigned char)s[b] & 0xC0) == 0x80) b++;
        std::string t = std::string(s.substr(0, a)) + dots + std::string(s.substr(b));
        if (text_.measure(f, size, t) <= max_w) return t;
    }
    (void)keep_end;
    return dots;
}

float Renderer::draw_text_in(FontId f, float size, const Rect& r, std::string_view s, Color c, Align a) {
    std::string t = ellipsize(f, size, s, r.w);
    float w = text_.measure(f, size, t);
    float lh = text_.line_height(f, size);
    float x = r.x;
    if (a == Align::Center) x = r.x + (r.w - w) / 2;
    else if (a == Align::Right) x = r.right() - w;
    float y = r.y + (r.h - lh) / 2;
    draw_text(f, size, std::round(x * scale_) / scale_, y, t, c);
    return w;
}

void Renderer::icon(const std::string& name, float x, float y, float size, Color c) {
    float s = scale_;
    auto& gi = text_.icon(name, size * s);
    if (!gi.valid) return;
    float px = std::round(x * s), py = std::round(y * s);
    Instance in{{px, py, float(gi.aw), float(gi.ah)},
                {float(gi.ax), float(gi.ay), float(gi.ax + gi.aw), float(gi.ay + gi.ah)},
                {c.r, c.g, c.b, c.a}, {0, 0, 0, 0}, {0, 0, 1, 0}};
    push_instance(in);
}

#ifndef __APPLE__
void Renderer::image(uint64_t key, const Image& img, const Rect& r, float alpha) {
    auto& I = *impl_;
    if (!img.valid() || r.w <= 0 || r.h <= 0) return;
    if (!key) key = hash_bytes(std::string_view(reinterpret_cast<const char*>(img.rgba.data()), img.rgba.size()));
    auto it = I.textures.find(key);
    if (it == I.textures.end()) {
        if (img.w > 8192 || img.h > 8192) return;
        it = I.textures.emplace(key, I.make_texture(img.w, img.h, img.rgba.data())).first;
    }
    it->second.last_used = I.frame_counter;
    float s = scale_;
    Instance in{{r.x * s, r.y * s, r.w * s, r.h * s}, {0, 0, 2048, 2048}, {1, 1, 1, alpha}, {0, 0, 0, 0}, {0, 0, 3, 0}};
    push_instance_tex(in, key);
}

#endif

void Renderer::push_clip(const Rect& r) { clips_.push_back(clips_.empty() ? r : clips_.back().intersect(r)); }
void Renderer::pop_clip() { if (!clips_.empty()) clips_.pop_back(); }
void Renderer::set_layer(int l) { layer_ = l; }

#ifndef __APPLE__
void Renderer::end_frame() {
    auto& I = *impl_;
    auto& vk = I.vk;
    if (!I.swapchain_ok) return;
    auto& F = I.frames[I.frame_index];
    vk.vkWaitForFences(I.device, 1, &F.fence, VK_TRUE, UINT64_MAX);
    VkResult r = vk.vkAcquireNextImageKHR(I.device, I.swapchain, UINT64_MAX, F.acquired, VK_NULL_HANDLE, &I.image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { I.create_swapchain(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    vk.vkResetFences(I.device, 1, &F.fence);

    // gather instances sorted by layer (stable keeps paint order within a layer)
    std::stable_sort(I.batches.begin(), I.batches.end(), [](const Batch& a, const Batch& b) { return a.layer < b.layer; });
    size_t total = 0;
    for (auto& b : I.batches) total += b.items.size();
    if (total * sizeof(Instance) > F.instances.size) {
        vk.vkDeviceWaitIdle(I.device);
        I.free_buffer(F.instances);
        F.instances = I.make_buffer(VkDeviceSize(total * 2 * sizeof(Instance)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    auto* dst = static_cast<Instance*>(F.instances.mapped);
    size_t pos = 0;
    for (auto& b : I.batches) {
        std::memcpy(dst + pos, b.items.data(), b.items.size() * sizeof(Instance));
        pos += b.items.size();
    }

    VkCommandBuffer cmd = F.cmd;
    vk.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &bi);
    I.upload_atlas(cmd, text_.atlas);

    VkClearValue cv{};
    cv.color = {{I.clear.r, I.clear.g, I.clear.b, 1}};
    VkRenderPassBeginInfo rpb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpb.renderPass = I.render_pass;
    rpb.framebuffer = I.framebuffers[I.image_index];
    rpb.renderArea = {{0, 0}, I.extent};
    rpb.clearValueCount = 1;
    rpb.pClearValues = &cv;
    vk.vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, I.pipeline);
    VkViewport vp{0, 0, float(I.extent.width), float(I.extent.height), 0, 1};
    vk.vkCmdSetViewport(cmd, 0, 1, &vp);
    float pc[2] = {float(I.extent.width), float(I.extent.height)};
    vk.vkCmdPushConstants(cmd, I.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, I.layout, 0, 1, &I.dset, 0, nullptr);
    VkDeviceSize zero = 0;
    vk.vkCmdBindVertexBuffers(cmd, 0, 1, &F.instances.buf, &zero);
    uint32_t first = 0;
    for (auto& b : I.batches) {
        if (b.items.empty()) continue;
        int x0 = std::max(0, int(std::floor(b.clip.x * scale_))), y0 = std::max(0, int(std::floor(b.clip.y * scale_)));
        int x1 = std::min(int(I.extent.width), int(std::ceil(b.clip.right() * scale_)));
        int y1 = std::min(int(I.extent.height), int(std::ceil(b.clip.bottom() * scale_)));
        if (x1 > x0 && y1 > y0) {
            VkDescriptorSet set = I.dset;
            if (b.tex) {
                auto t = I.textures.find(b.tex);
                if (t != I.textures.end()) set = t->second.set;
            }
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, I.layout, 0, 1, &set, 0, nullptr);
            VkRect2D sc{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
            vk.vkCmdSetScissor(cmd, 0, 1, &sc);
            vk.vkCmdDraw(cmd, 6, uint32_t(b.items.size()), 0, first);
        }
        first += uint32_t(b.items.size());
    }
    vk.vkCmdEndRenderPass(cmd);

    if (I.capture_pending && I.supports_capture) {
        VkDeviceSize need = VkDeviceSize(I.extent.width) * I.extent.height * 4;
        if (!I.readback.buf || I.readback.size < need) {
            I.free_buffer(I.readback);
            I.readback = I.make_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = I.images[I.image_index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy reg{};
        reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reg.imageExtent = {I.extent.width, I.extent.height, 1};
        vk.vkCmdCopyImageToBuffer(cmd, I.images[I.image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, I.readback.buf, 1, &reg);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                                0, nullptr, 1, &b);
        I.capture_w = int(I.extent.width);
        I.capture_h = int(I.extent.height);
        I.capture_pending = false;
        I.capture_ready = true;
    }
    vk.vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &F.acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &F.rendered;
    vk.vkQueueSubmit(I.queue, 1, &si, F.fence);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &F.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &I.swapchain;
    pi.pImageIndices = &I.image_index;
    r = vk.vkQueuePresentKHR(I.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) I.swapchain_ok = false;
    I.frame_index = (I.frame_index + 1) % Impl::FRAMES;
}

bool Renderer::capture(std::vector<uint8_t>& rgba, int& w, int& h) {
    auto& I = *impl_;
    if (!I.supports_capture) return false;
    if (!I.capture_ready) {
        I.capture_pending = true;
        return false;
    }
    I.vk.vkQueueWaitIdle(I.queue);
    w = I.capture_w;
    h = I.capture_h;
    rgba.resize(size_t(w) * size_t(h) * 4);
    auto* src = static_cast<const uint8_t*>(I.readback.mapped);
    bool bgr = I.format == VK_FORMAT_B8G8R8A8_UNORM || I.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < size_t(w) * size_t(h); i++) {
        rgba[4 * i + 0] = src[4 * i + (bgr ? 2 : 0)];
        rgba[4 * i + 1] = src[4 * i + 1];
        rgba[4 * i + 2] = src[4 * i + (bgr ? 0 : 2)];
        rgba[4 * i + 3] = 255;
    }
    I.capture_ready = false;
    return true;
}

#endif

}  // namespace gfx
