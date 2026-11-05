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
    VkCommandBuffer cmd;             /* the frame's (one of two: the GPU draws one frame while the next is made) */
    VkCommandBuffer cmds[2];
    VkFence fences[2];
    int frame;                       /* which of the two */
    void (*frame_reused)(int frame);  /* the frame's resources are free again (the GPU is done with them) */
    int recording;
    /* the window's, if any */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    uint32_t swap_count;
    VkImage swap_images[8];
    VkImageView swap_views[8];
    VkSemaphore acquired, rendered;  /* the frame's (acquired) and the swapchain image's (rendered) */
    VkSemaphore acquired_s[2], rendered_s[8];
    /* extended dynamic state 3 */
    PFN_vkCmdSetColorBlendEnableEXT SetColorBlendEnable;
    PFN_vkCmdSetColorBlendEquationEXT SetColorBlendEquation;
    PFN_vkCmdSetColorWriteMaskEXT SetColorWriteMask;
    int (*before_submit)(VkCommandBuffer *out); /* commands to submit before the frame's, up to 2 (render_shadow.c: shadows) */
    int output_layer;                /* a vertex shader can choose the layer it draws into */
    int tessellation;                /* tessellation shaders can be used */
    int clip_distance;               /* clip distances and depth clamping can be used */
    int barycentric;                 /* the pixel stage can read its triangle's corners' values */
    int anisotropy;                  /* anisotropic filtering can be used */
    int present_timing;              /* the driver says when frames go out, for this window (pacing.c) */
    int present_at_time;             /* and a present can name when it is to go out */
} VkContext;

extern VkContext vk;

/* window: an SDL_Window (with SDL_WINDOW_VULKAN) or NULL to render offscreen */
int vk_init(void *window);
void vk_recreate_swapchain(void);

VkBuf vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int host);
void vk_buffer_free(VkBuf *b);
VkImg vk_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
VkImg vk_image_mips(uint32_t w, uint32_t h, uint32_t levels, VkFormat format, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect);
VkImg vk_image_layers(uint32_t w, uint32_t h, uint32_t layers, VkImageCreateFlags flags, VkFormat format,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect);
void vk_image_free(VkImg *i);
void vk_transition(VkImg *i, VkImageLayout to);
VkShaderModule vk_shader(const uint32_t *code, size_t size);

/* The frame's command buffer: begun on first use, submitted (and waited for) by vk_submit */
VkCommandBuffer vk_cmd(void);
void vk_submit(VkSemaphore wait, VkSemaphore signal);
void vk_finish(void);                /* wait for everything submitted */

#endif
