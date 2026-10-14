/* The Vulkan context: see vk.h. Vulkan 1.3 (dynamic rendering, synchronization2) with descriptor indexing (bindless
 * textures) and extended dynamic state 3 (blending set per draw), so the renderer needs a single pipeline.
 * MHFU_VK_VALIDATE=1 turns the validation layer on. */
#include "vk.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VkContext vk;
static SDL_Window *sdl_window;

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "Vulkan: %s: %d\n", #x, r_); exit(1); } } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                VkDebugUtilsMessageTypeFlagsEXT type,
                                                const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    return VK_FALSE;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "Vulkan: no suitable memory type\n");
    exit(1);
}

int vk_init(void *window) {
    sdl_window = window;
    int validate = getenv("MHFU_VK_VALIDATE") != NULL;
    const char *exts[16];
    uint32_t n_exts = 0;
    if (window) {
        Uint32 n;
        const char *const *sdl = SDL_Vulkan_GetInstanceExtensions(&n);
        for (Uint32 i = 0; i < n && n_exts < 14; i++)
            exts[n_exts++] = sdl[i];
    }
    if (validate)
        exts[n_exts++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "mhfu-recomp", 1, "mhfu-recomp", 1,
                             VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, validate ? 1u : 0u, layers,
                                n_exts, exts};
    if (vkCreateInstance(&ici, NULL, &vk.instance) != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: no instance\n");
        return 0;
    }
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT create =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, on_message,
            NULL};
        VkDebugUtilsMessengerEXT m;
        if (create)
            create(vk.instance, &mci, NULL, &m);
    }
    if (window && !SDL_Vulkan_CreateSurface(window, vk.instance, NULL, &vk.surface)) {
        fprintf(stderr, "Vulkan: no surface: %s\n", SDL_GetError());
        return 0;
    }

    /* the first device with a graphics queue (that can present, with a window), discrete preferred */
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(vk.instance, &n, NULL);
    VkPhysicalDevice gpus[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(vk.instance, &n, gpus);
    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qn, NULL);
        VkQueueFamilyProperties q[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qn, q);
        for (uint32_t k = 0; k < qn; k++) {
            VkBool32 present = VK_TRUE;
            if (vk.surface)
                vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], k, vk.surface, &present);
            if ((q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                VkPhysicalDeviceProperties p;
                vkGetPhysicalDeviceProperties(gpus[i], &p);
                if (best < 0 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    best = (int)i;
                    vk.queue_family = k;
                }
                break;
            }
        }
    }
    if (best < 0) {
        fprintf(stderr, "Vulkan: no usable device\n");
        return 0;
    }
    vk.gpu = gpus[best];

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    eds3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    eds3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    eds3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &eds3};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;    /* discard (the alpha test) */
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    const char *dev_exts[] = {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, vk.queue_family, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL,
                              vk.surface ? 2u : 1u, dev_exts, NULL};
    CHECK(vkCreateDevice(vk.gpu, &dci, NULL, &vk.device));
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
    vk.SetColorBlendEnable = (PFN_vkCmdSetColorBlendEnableEXT)vkGetDeviceProcAddr(vk.device, "vkCmdSetColorBlendEnableEXT");
    vk.SetColorBlendEquation =
        (PFN_vkCmdSetColorBlendEquationEXT)vkGetDeviceProcAddr(vk.device, "vkCmdSetColorBlendEquationEXT");
    vk.SetColorWriteMask = (PFN_vkCmdSetColorWriteMaskEXT)vkGetDeviceProcAddr(vk.device, "vkCmdSetColorWriteMaskEXT");

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, vk.queue_family};
    CHECK(vkCreateCommandPool(vk.device, &pci, NULL, &vk.pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, vk.pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    CHECK(vkAllocateCommandBuffers(vk.device, &cai, vk.cmds));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (int i = 0; i < 2; i++) {
        CHECK(vkCreateFence(vk.device, &fci, NULL, &vk.fences[i]));
        CHECK(vkCreateSemaphore(vk.device, &sci, NULL, &vk.acquired_s[i]));
    }
    for (int i = 0; i < 8; i++)
        CHECK(vkCreateSemaphore(vk.device, &sci, NULL, &vk.rendered_s[i]));
    vk.cmd = vk.cmds[0];
    vk.acquired = vk.acquired_s[0];
    vk.rendered = vk.rendered_s[0];
    if (vk.surface)
        vk_recreate_swapchain();
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(vk.gpu, &p);
    fprintf(stderr, "Vulkan: %s\n", p.deviceName);
    return 1;
}

void vk_recreate_swapchain(void) {
    vkDeviceWaitIdle(vk.device);
    for (uint32_t i = 0; i < vk.swap_count; i++)
        vkDestroyImageView(vk.device, vk.swap_views[i], NULL);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &nf, NULL);
    VkSurfaceFormatKHR formats[32];
    if (nf > 32) nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &nf, formats);
    VkSurfaceFormatKHR fmt = formats[0];
    for (uint32_t i = 0; i < nf; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            fmt = formats[i];
    vk.swap_format = fmt.format;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w, h;
        SDL_GetWindowSizeInPixels(sdl_window, &w, &h);
        ext.width = (uint32_t)w;
        ext.height = (uint32_t)h;
    }
    vk.swap_extent = ext;
    /* mailbox if there is one: the game paces itself, the display should not hold it back */
    uint32_t np = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &np, NULL);
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    if (np > 8) np = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &np, modes);
    for (uint32_t i = 0; i < np; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            mode = modes[i];
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    VkSwapchainKHR old = vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, vk.surface, count,
        fmt.format, fmt.colorSpace, ext, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE,
        old};
    CHECK(vkCreateSwapchainKHR(vk.device, &sci, NULL, &vk.swapchain));
    if (old)
        vkDestroySwapchainKHR(vk.device, old, NULL);
    vk.swap_count = 8;
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swap_count, vk.swap_images);
    for (uint32_t i = 0; i < vk.swap_count; i++) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, vk.swap_images[i],
            VK_IMAGE_VIEW_TYPE_2D, fmt.format, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(vk.device, &vci, NULL, &vk.swap_views[i]));
    }
}

VkBuf vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int host) {
    VkBuf b = {0};
    b.size = size;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE,
                              0, NULL};
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, b.buffer, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
        memory_type(req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &b.memory));
    CHECK(vkBindBufferMemory(vk.device, b.buffer, b.memory, 0));
    if (host)
        CHECK(vkMapMemory(vk.device, b.memory, 0, size, 0, &b.map));
    return b;
}

void vk_buffer_free(VkBuf *b) {
    if (b->buffer)
        vkDestroyBuffer(vk.device, b->buffer, NULL);
    if (b->memory)
        vkFreeMemory(vk.device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

VkImg vk_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImg i = {0};
    i.format = format;
    i.width = w;
    i.height = h;
    i.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    i.aspect = aspect;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, format, {w, h, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(vk.device, &ici, NULL, &i.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, i.image, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &i.memory));
    CHECK(vkBindImageMemory(vk.device, i.image, i.memory, 0));
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, i.image, VK_IMAGE_VIEW_TYPE_2D,
                                 format, {0}, {aspect, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(vk.device, &vci, NULL, &i.view));
    return i;
}

void vk_image_free(VkImg *i) {
    if (i->view)
        vkDestroyImageView(vk.device, i->view, NULL);
    if (i->image)
        vkDestroyImage(vk.device, i->image, NULL);
    if (i->memory)
        vkFreeMemory(vk.device, i->memory, NULL);
    memset(i, 0, sizeof *i);
}

/* A full barrier into the new layout (simple, not minimal) */
void vk_transition(VkImg *i, VkImageLayout to) {
    if (i->layout == to)
        return;
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        i->layout, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, i->image, {i->aspect, 0, 1, 0, 1}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(vk_cmd(), &d);
    i->layout = to;
}

VkShaderModule vk_shader(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(vk.device, &ci, NULL, &m));
    return m;
}

VkCommandBuffer vk_cmd(void) {
    if (!vk.recording) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        CHECK(vkBeginCommandBuffer(vk.cmd, &bi));
        vk.recording = 1;
    }
    return vk.cmd;
}

/* Submit the frame's commands and wait for them (the next frame reuses the command buffer and the upload space) */
void vk_submit(VkSemaphore wait, VkSemaphore signal) {
    vk_cmd();
    CHECK(vkEndCommandBuffer(vk.cmd));
    vk.recording = 0;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkCommandBuffer cmds[2] = {VK_NULL_HANDLE, vk.cmd};
    cmds[0] = vk.before_submit ? vk.before_submit() : VK_NULL_HANDLE;
    int first = cmds[0] ? 0 : 1;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, wait ? 1u : 0u, wait ? &wait : NULL, &stage,
                       (uint32_t)(2 - first), &cmds[first], signal ? 1u : 0u, signal ? &signal : NULL};
    CHECK(vkResetFences(vk.device, 1, &vk.fences[vk.frame]));
    CHECK(vkQueueSubmit(vk.queue, 1, &si, vk.fences[vk.frame]));
    /* on to the other frame's resources, once the GPU is done with them (the frame before this one) */
    vk.frame ^= 1;
    CHECK(vkWaitForFences(vk.device, 1, &vk.fences[vk.frame], VK_TRUE, UINT64_MAX));
    vk.cmd = vk.cmds[vk.frame];
    vk.acquired = vk.acquired_s[vk.frame];
    CHECK(vkResetCommandBuffer(vk.cmd, 0));
    if (vk.frame_reused)
        vk.frame_reused(vk.frame);
}

void vk_finish(void) {
    CHECK(vkQueueWaitIdle(vk.queue));
}
