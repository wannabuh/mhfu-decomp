/* The Vulkan context the renderer (render.c) draws with: device, queue, the window's swapchain (if there is a window),
 * and helpers for buffers, images and the frame's command buffer. */
#ifndef VK_H
#define VK_H

#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *map;                       /* host-visible buffers are mapped */
    VkDeviceSize size;
} VkBuf;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    VkImageLayout layout;
    VkImageAspectFlags aspect;
} VkImg;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool pool;
    VkCommandBuffer cmd;             /* the frame's */
    VkFence fence;
    int recording;
    /* the window's, if any */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    uint32_t swap_count;
    VkImage swap_images[8];
    VkImageView swap_views[8];
    VkSemaphore acquired, rendered;
    /* extended dynamic state 3 */
    PFN_vkCmdSetColorBlendEnableEXT SetColorBlendEnable;
    PFN_vkCmdSetColorBlendEquationEXT SetColorBlendEquation;
    PFN_vkCmdSetColorWriteMaskEXT SetColorWriteMask;
} VkContext;

extern VkContext vk;

/* window: an SDL_Window (with SDL_WINDOW_VULKAN) or NULL to render offscreen */
int vk_init(void *window);
void vk_recreate_swapchain(void);

VkBuf vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int host);
void vk_buffer_free(VkBuf *b);
VkImg vk_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
void vk_image_free(VkImg *i);
void vk_transition(VkImg *i, VkImageLayout to);
VkShaderModule vk_shader(const uint32_t *code, size_t size);

/* The frame's command buffer: begun on first use, submitted (and waited for) by vk_submit */
VkCommandBuffer vk_cmd(void);
void vk_submit(VkSemaphore wait, VkSemaphore signal);

#endif
