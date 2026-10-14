/* The Vulkan renderer: see render.h.
 *
 * Render targets: one per framebuffer address in VRAM, 512 x 512 PSP pixels at MHFU_SCALE times the resolution
 * (default 2), colour (RGBA8) and depth. Primitives are batched into draws while the GE's state stays the same; a draw's
 * state is set dynamically (one pipeline): blending, depth test and write, colour write mask, scissor; the texture is an
 * index into a bindless array, with the texture function and alpha test in push constants. Textures are decoded on the
 * CPU (by gpu.c) and cached by address, format, size and a hash of their data (checked once a frame); a texture whose
 * address is a render target's samples that render target. Frames are submitted and waited for at each flip.
 *
 * Not yet: the framebuffers' contents in VRAM for the CPU (nothing reads them back but frame dumps), block transfers
 * into framebuffers, stencil, logic ops, mipmaps, the blend factors the hardware lacks (doubled alphas). */
#include "render.h"

#include "hle.h"
#include "vk.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "ge.vert.inc"
#include "ge.frag.inc"
#include "present.vert.inc"
#include "present.frag.inc"

int render_on;

#define RT_SIZE 512                 /* PSP pixels, square */
#define MAX_TARGETS 32
#define MAX_TEXTURES 4096
#define VERTEX_BYTES (64u << 20)
#define STAGING_BYTES (64u << 20)

typedef struct { float x, y, z, w, u, v; uint32_t color; float fog; } GpuVertex;

typedef struct {
    uint32_t addr;                  /* VRAM address, mirror bits off */
    VkImg color, depth;
    int used, cleared;
    uint32_t sample_slot;           /* its slot in the texture array, when sampled */
} Target;

typedef struct {
    uint64_t key, hash;
    uint32_t addr;
    int w, h;
    VkImg img;
    uint32_t slot;
    unsigned long checked;          /* the frame it was last checked in */
    unsigned long used;
} Texture;

/* A draw's state: everything that, changed, needs a new draw */
typedef struct {
    Target *target;
    uint32_t tex_slot;
    float tex_w, tex_h;
    uint32_t flags, alpha_test, env_color, fog_color;
    VkBool32 blend;
    VkColorBlendEquationEXT eq;
    float blend_const[4];
    VkBool32 depth_test, depth_write;
    VkCompareOp depth_op;
    VkColorComponentFlags write_mask;
    VkRect2D scissor;
} DrawState;

static int scale = 2;
static Target targets[MAX_TARGETS];
static Texture *textures;
static int ntextures;
static VkBuf vertices, staging;
static VkDeviceSize vertex_used, staging_used;
static VkPipeline pipeline, present_pipeline;
static VkPipelineLayout layout, present_layout;
static VkDescriptorSetLayout set_layout, present_set_layout;
static VkDescriptorPool pool;
static VkDescriptorSet set, present_set;
static VkSampler samplers[8], present_sampler;
static unsigned long frame_no = 1;

static Target *rendering;            /* the target being rendered into, or NULL */
static DrawState cur;                /* the current primitive's state */
static DrawState last;               /* the last draw's, as set in the command buffer */
static int last_valid;
static VkDeviceSize batch_start;     /* vertices of the batch not drawn yet */
static VkBuf readback;

/* -- setup -------------------------------------------------------------------------------------------------------- */

static VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout pl, VkFormat color,
                                VkFormat depth, int ge) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
    };
    VkVertexInputBindingDescription bind = {0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24},
        {3, 0, VK_FORMAT_R32_SFLOAT, 28},
    };
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
                                               ge ? 1u : 0u, &bind, ge ? 4u : 0u, attrs};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1,
                                            NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att = {0};
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
                                              VK_FALSE, VK_LOGIC_OP_COPY, 1, &att, {0}};
    VkDynamicState dyn_ge[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
        VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
    VkDynamicState dyn_present[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0,
                                           ge ? 9u : 2u, ge ? dyn_ge : dyn_present};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &color, depth,
                                        VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vi, &ia,
                                        NULL, &vp, &rs, &ms, ge ? &ds : NULL, &cb, &dy, pl, VK_NULL_HANDLE, 0,
                                        VK_NULL_HANDLE, -1};
    VkPipeline p;
    if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p) != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: pipeline creation failed\n");
        exit(1);
    }
    return p;
}

int render_init(void *sdl_window) {
    const char *s = getenv("MHFU_SCALE");
    if (s && atoi(s) >= 1 && atoi(s) <= 8)
        scale = atoi(s);
    if (!vk_init(sdl_window))
        return 0;

    /* bindless textures: one array, filled as textures are made */
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES,
                                      VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                  VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                  VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 1, &bf};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi,
                                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &b};
    vkCreateDescriptorSetLayout(vk.device, &dl, NULL, &set_layout);
    VkDescriptorSetLayoutBinding pb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       NULL};
    VkDescriptorSetLayoutCreateInfo pdl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &pb};
    vkCreateDescriptorSetLayout(vk.device, &pdl, NULL, &present_set_layout);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES + 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                      VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 2, 1, &ps};
    vkCreateDescriptorPool(vk.device, &dpi, NULL, &pool);
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &set_layout};
    vkAllocateDescriptorSets(vk.device, &dai, &set);
    dai.pSetLayouts = &present_set_layout;
    vkAllocateDescriptorSets(vk.device, &dai, &present_set);

    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 28};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout, 1, &pr};
    vkCreatePipelineLayout(vk.device, &pli, NULL, &layout);
    VkPushConstantRange ppr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo ppli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &present_set_layout,
                                       1, &ppr};
    vkCreatePipelineLayout(vk.device, &ppli, NULL, &present_layout);

    VkShaderModule vs = vk_shader(ge_vert, sizeof ge_vert), fs = vk_shader(ge_frag, sizeof ge_frag);
    pipeline = make_pipeline(vs, fs, layout, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 1);
    if (vk.surface) {
        VkShaderModule pvs = vk_shader(present_vert, sizeof present_vert);
        VkShaderModule pfs = vk_shader(present_frag, sizeof present_frag);
        present_pipeline = make_pipeline(pvs, pfs, present_layout, vk.swap_format, VK_FORMAT_UNDEFINED, 0);
    }

    /* samplers: nearest or linear, each axis repeating or clamped */
    for (int i = 0; i < 8; i++) {
        VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = (i & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.addressModeU = (i & 2) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = (i & 4) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = 0;
        vkCreateSampler(vk.device, &sci, NULL, &samplers[i]);
    }
    VkSamplerCreateInfo psi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    psi.magFilter = psi.minFilter = VK_FILTER_LINEAR;
    psi.addressModeU = psi.addressModeV = psi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk.device, &psi, NULL, &present_sampler);

    vertices = vk_buffer(VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
    staging = vk_buffer(STAGING_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
    readback = vk_buffer((VkDeviceSize)RT_SIZE * scale * RT_SIZE * scale * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    textures = calloc(MAX_TEXTURES, sizeof *textures);
    render_on = 1;
    fprintf(stderr, "renderer: Vulkan, %dx internal resolution\n", scale);
    return 1;
}

/* -- render targets ----------------------------------------------------------------------------------------------- */

static uint32_t next_slot = 0;

static void write_slot(uint32_t slot, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo ii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, slot, 1,
                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &w, 0, NULL);
}

static Target *target(uint32_t addr, int create) {
    addr &= 0x041FFFF0u;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (targets[i].used && targets[i].addr == addr)
            return &targets[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (!targets[i].used) {
            Target *t = &targets[i];
            uint32_t px = (uint32_t)(RT_SIZE * scale);
            t->used = 1;
            t->addr = addr;
            t->color = vk_image(px, px, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
            t->depth = vk_image(px, px, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
            t->sample_slot = next_slot++;
            write_slot(t->sample_slot, t->color.view, samplers[1 | 2 | 4]);
            return t;
        }
    fprintf(stderr, "renderer: too many render targets\n");
    exit(1);
}

static void flush(void);

static void end_rendering(void) {
    if (!rendering)
        return;
    flush();
    vkCmdEndRendering(vk_cmd());
    rendering = NULL;
}

static void begin_rendering(Target *t) {
    if (rendering == t)
        return;
    end_rendering();
    vk_transition(&t->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vk_transition(&t->depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, t->color.view,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        t->cleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    VkRenderingAttachmentInfo da = ca;
    da.imageView = t->depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.clearValue.depthStencil.depth = 0.0f;
    uint32_t px = (uint32_t)(RT_SIZE * scale);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {px, px}}, 1, 0, 1, &ca, &da, NULL};
    VkCommandBuffer cmd = vk_cmd();
    vkCmdBeginRendering(cmd, &ri);
    t->cleared = 1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &zero);
    VkViewport vp = {0, 0, (float)px, (float)px, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    rendering = t;
    last_valid = 0;
}

/* -- textures ----------------------------------------------------------------------------------------------------- */

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (size_t i = n & ~(size_t)7; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* The current texture's slot: a render target's, or a cached texture's (made or updated as needed) */
static uint32_t texture_slot(const uint32_t *reg, int *w_out, int *h_out) {
    int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
    *w_out = w;
    *h_out = h;
    uint32_t addr = (reg[0xA0] & 0xFFFFF0) | ((reg[0xA8] & 0x0F0000) << 8);
    int fmt = reg[0xC3] & 0xF;
    uint32_t bw = reg[0xA8] & 0x7FF;
    int filt = ((reg[0xC6] >> 8) & 1) || (reg[0xC6] & 1);
    int sampler = filt | ((reg[0xC7] & 1) << 1) | (((reg[0xC7] >> 8) & 1) << 2);

    Target *rt = (addr & 0x0F000000u) == 0x04000000u ? target(addr, 0) : NULL;
    if (rt) {                       /* render to texture: its size in PSP pixels is the target's */
        *w_out = *h_out = RT_SIZE;
        (void)w; (void)h;
        return rt->sample_slot;
    }

    uint64_t key = (uint64_t)addr | (uint64_t)fmt << 32 | (uint64_t)(reg[0xB8] & 0xFFFF) << 36 |
                   (uint64_t)sampler << 52 | (uint64_t)(reg[0xC2] & 1) << 55 | (uint64_t)bw << 56;
    int bits = fmt == 3 || fmt == 7 ? 32 : fmt == 4 ? 4 : fmt == 5 ? 8 : 16;
    size_t bytes = (size_t)(bw ? bw : (uint32_t)w) * (size_t)h * (size_t)bits / 8;
    /* consecutive draws mostly use the same texture: the last one found is tried first */
    static int last_found = -1;
    Texture *t = NULL;
    if (last_found >= 0 && last_found < ntextures && textures[last_found].key == key)
        t = &textures[last_found];
    else
        for (int i = 0; i < ntextures; i++)
            if (textures[i].key == key) {
                t = &textures[i];
                last_found = i;
                break;
            }
    if (t && t->checked == frame_no) {
        t->used = frame_no;
        return t->slot;
    }
    uint64_t hash = hash_bytes(MEM(addr), bytes, 0xCBF29CE484222325ull);
    if (fmt >= 4 && fmt <= 7) {     /* the palette too */
        uint32_t caddr = (reg[0xB0] & 0xFFFFF0) | ((reg[0xB1] & 0x0F0000) << 8);
        hash = hash_bytes(MEM(caddr), (reg[0xC4] & 0x3F) * 32, hash) ^ reg[0xC5] * 0x9E3779B97F4A7C15ull;
    }
    if (t && t->hash == hash) {
        t->checked = t->used = frame_no;
        return t->slot;
    }
    if (!t) {
        if (ntextures >= MAX_TEXTURES - MAX_TARGETS - 1) {
            /* evict the least recently used */
            int lru = 0;
            for (int i = 1; i < ntextures; i++)
                if (textures[i].used < textures[lru].used)
                    lru = i;
            t = &textures[lru];
            vk_image_free(&t->img);
        } else {
            t = &textures[ntextures++];
            t->slot = next_slot++;
        }
        t->key = key;
        t->w = t->h = 0;
    }
    /* decode and upload */
    size_t size = (size_t)w * (size_t)h * 4;
    if (staging_used + size > STAGING_BYTES) {
        fprintf(stderr, "renderer: out of staging space this frame\n");
        return t->slot;
    }
    gpu_decode_texture((uint32_t *)((uint8_t *)staging.map + staging_used), w, h);
    if (t->w != w || t->h != h) {
        vk_image_free(&t->img);
        t->img = vk_image((uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        t->w = w;
        t->h = h;
    }
    Target *was = rendering;
    end_rendering();
    vk_transition(&t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {staging_used, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                              {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyBufferToImage(vk_cmd(), staging.buffer, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_transition(&t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    staging_used += (size + 255) & ~(size_t)255;
    write_slot(t->slot, t->img.view, samplers[sampler]);
    t->hash = hash;
    t->addr = addr;
    t->checked = t->used = frame_no;
    if (was)
        begin_rendering(was);
    return t->slot;
}

/* -- state -------------------------------------------------------------------------------------------------------- */

static const VkBlendFactor src_factors[16] = {
    VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR,
    VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE};
static const VkBlendFactor dst_factors[16] = {
    VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR,
    VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO};
static const VkBlendOp blend_ops[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
    VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
static const VkCompareOp depth_ops[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL,
    VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
    VK_COMPARE_OP_GREATER_OR_EQUAL};

static void unpack_rgb(uint32_t c, float *f) {
    f[0] = (float)(c & 255) / 255.0f;
    f[1] = (float)((c >> 8) & 255) / 255.0f;
    f[2] = (float)((c >> 16) & 255) / 255.0f;
    f[3] = 1.0f;
}

static uint32_t override_slot = 0xFFFFFFFEu;
static int override_w, override_h;

void render_override_texture(uint32_t slot, int w, int h) {
    override_slot = slot;
    override_w = w;
    override_h = h;
}

uint32_t render_texture(const uint32_t *reg, int *w, int *h) {
    return texture_slot(reg, w, h);
}

void render_prim_begin(const uint32_t *reg) {
    DrawState s;
    memset(&s, 0, sizeof s);
    uint32_t fb = 0x04000000u | ((reg[0x9C] & 0xFFFFF0) | ((reg[0x9D] & 0xFF0000) << 8)) % 0x200000u;
    s.target = target(fb, 1);
    int clear = reg[0xD3] & 1;
    s.tex_slot = 0xFFFFFFFFu;
    if (!clear && (reg[0x1E] & 1)) {
        int w, h;
        if (override_slot != 0xFFFFFFFEu) {
            s.tex_slot = override_slot;
            w = override_w;
            h = override_h;
        } else {
            s.tex_slot = texture_slot(reg, &w, &h);
        }
        s.tex_w = (float)w;
        s.tex_h = (float)h;
    }
    s.flags = (reg[0xC9] & 7) | (((reg[0xC9] >> 8) & 1) << 3) | (((reg[0xC9] >> 16) & 1) << 4) |
              (!clear && (reg[0x22] & 1) ? 32u : 0u) | (clear ? 64u : 0u) |
              (!clear && (reg[0x1F] & 1) && !((reg[0x12] >> 23) & 1) ? 128u : 0u);
    s.fog_color = reg[0xCF];
    s.alpha_test = reg[0xDB];
    s.env_color = reg[0xCA];
    if (clear) {
        uint32_t mode = reg[0xD3] >> 8;
        s.write_mask = ((mode & 1) ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT : 0) |
                       ((mode & 2) ? VK_COLOR_COMPONENT_A_BIT : 0);
        s.depth_test = (mode & 4) ? VK_TRUE : VK_FALSE;
        s.depth_write = (mode & 4) ? VK_TRUE : VK_FALSE;
        s.depth_op = VK_COMPARE_OP_ALWAYS;
        s.blend = VK_FALSE;
    } else {
        s.write_mask = 0xF;
        s.depth_test = (reg[0x23] & 1) ? VK_TRUE : VK_FALSE;
        s.depth_write = s.depth_test && !(reg[0xE7] & 1);
        s.depth_op = depth_ops[reg[0xDE] & 7];
        s.blend = (reg[0x21] & 1) ? VK_TRUE : VK_FALSE;
        if (s.blend) {
            uint32_t b = reg[0xDF];
            int fs = b & 15, fd = (b >> 4) & 15;
            s.eq.srcColorBlendFactor = src_factors[fs];
            s.eq.dstColorBlendFactor = dst_factors[fd];
            s.eq.colorBlendOp = blend_ops[(b >> 8) & 7];
            s.eq.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            s.eq.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            s.eq.alphaBlendOp = VK_BLEND_OP_ADD;
            /* the fixed colours: Vulkan has one constant. Fixed white / black are one and zero; both fixed and adding up
             * to white (a fade: x of the source, 1 - x of what is there) are the constant and one minus it */
            uint32_t fa = reg[0xE0] & 0xFFFFFF, fbx = reg[0xE1] & 0xFFFFFF;
            if (fs == 10 && fa == 0xFFFFFF) s.eq.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            if (fs == 10 && fa == 0) s.eq.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            if (fd == 10 && fbx == 0xFFFFFF) s.eq.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            if (fd == 10 && fbx == 0) s.eq.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            int src_const = s.eq.srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR;
            int dst_const = s.eq.dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR;
            if (src_const && dst_const) {
                int complement = 1;
                for (int ch = 0; ch < 24; ch += 8) {
                    int a = (int)((fa >> ch) & 255), b = (int)((fbx >> ch) & 255);
                    if (a + b < 254 || a + b > 256)
                        complement = 0;
                }
                if (complement) {
                    s.eq.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
                } else {
                    static int warned;
                    if (!warned++)
                        fprintf(stderr, "renderer: blending with two unrelated fixed colours (%06X, %06X): approximated\n",
                                fa, fbx);
                }
            }
            unpack_rgb(src_const ? fa : fbx, s.blend_const);
        }
    }
    int sx0 = reg[0xD4] & 0x3FF, sy0 = (reg[0xD4] >> 10) & 0x3FF;
    int sx1 = (reg[0xD5] & 0x3FF) + 1, sy1 = ((reg[0xD5] >> 10) & 0x3FF) + 1;
    if (sx1 > RT_SIZE) sx1 = RT_SIZE;
    if (sy1 > RT_SIZE) sy1 = RT_SIZE;
    if (sx1 < sx0) sx1 = sx0;
    if (sy1 < sy0) sy1 = sy0;
    s.scissor = (VkRect2D){{sx0 * scale, sy0 * scale}, {(uint32_t)((sx1 - sx0) * scale), (uint32_t)((sy1 - sy0) * scale)}};
    if (memcmp(&s, &cur, sizeof s)) {
        flush();
        cur = s;
    }
}

/* Draw what has been batched with the current state */
static void flush(void) {
    VkDeviceSize n = (vertex_used - batch_start) / sizeof(GpuVertex);
    if (!n || !rendering)
        return;
    VkCommandBuffer cmd = vk_cmd();
    DrawState *s = &cur;
    if (!last_valid || memcmp(&s->scissor, &last.scissor, sizeof s->scissor))
        vkCmdSetScissor(cmd, 0, 1, &s->scissor);
    if (!last_valid || s->depth_test != last.depth_test)
        vkCmdSetDepthTestEnable(cmd, s->depth_test);
    if (!last_valid || s->depth_write != last.depth_write)
        vkCmdSetDepthWriteEnable(cmd, s->depth_write);
    if (!last_valid || s->depth_op != last.depth_op)
        vkCmdSetDepthCompareOp(cmd, s->depth_op);
    if (!last_valid || s->blend != last.blend)
        vk.SetColorBlendEnable(cmd, 0, 1, &s->blend);
    if (!last_valid || memcmp(&s->eq, &last.eq, sizeof s->eq))
        vk.SetColorBlendEquation(cmd, 0, 1, &s->eq);
    if (!last_valid || s->write_mask != last.write_mask)
        vk.SetColorWriteMask(cmd, 0, 1, &s->write_mask);
    if (!last_valid || memcmp(s->blend_const, last.blend_const, sizeof s->blend_const))
        vkCmdSetBlendConstants(cmd, s->blend_const);
    struct { float w, h; uint32_t slot, flags, alpha, env, fog; } pc = {s->tex_w ? s->tex_w : 1,
        s->tex_h ? s->tex_h : 1, s->tex_slot, s->flags, s->alpha_test, s->env_color, s->fog_color};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cmd, (uint32_t)n, 1, (uint32_t)(batch_start / sizeof(GpuVertex)), 0);
    last = *s;
    last_valid = 1;
    batch_start = vertex_used;
}

/* -- primitives --------------------------------------------------------------------------------------------------- */

static void put(const RVertex *v, float x, float y, float u, float vv, int sprite) {
    if (vertex_used + sizeof(GpuVertex) > VERTEX_BYTES)
        return;
    GpuVertex *g = (GpuVertex *)((uint8_t *)vertices.map + vertex_used);
    float w = sprite ? 1.0f : 1.0f / v->rw;
    float z = v->z / 65535.0f;
    z = z < 0 ? 0 : z > 1 ? 1 : z;
    g->x = (x / (RT_SIZE / 2.0f) - 1.0f) * w;
    g->y = (y / (RT_SIZE / 2.0f) - 1.0f) * w;
    g->z = z * w;
    g->w = w;
    g->u = u;
    g->v = vv;
    uint32_t r = v->r < 0 ? 0 : v->r > 255 ? 255 : (uint32_t)v->r, gg = v->g < 0 ? 0 : v->g > 255 ? 255 : (uint32_t)v->g;
    uint32_t b = v->b < 0 ? 0 : v->b > 255 ? 255 : (uint32_t)v->b, a = v->a < 0 ? 0 : v->a > 255 ? 255 : (uint32_t)v->a;
    g->color = r | gg << 8 | b << 16 | a << 24;
    g->fog = v->fog;
    vertex_used += sizeof(GpuVertex);
}

void render_triangle(const RVertex *a, const RVertex *b, const RVertex *c) {
    begin_rendering(cur.target);
    put(a, a->x, a->y, a->u, a->v, 0);
    put(b, b->x, b->y, b->u, b->v, 0);
    put(c, c->x, c->y, c->u, c->v, 0);
}

/* A sprite: the rectangle between two corners, as two triangles, in the second corner's colour and depth */
void render_sprite(const RVertex *a, const RVertex *b) {
    begin_rendering(cur.target);
    RVertex v = *b;
    v.rw = 1.0f;
    put(&v, a->x, a->y, a->u, a->v, 1);
    put(&v, b->x, a->y, b->u, a->v, 1);
    put(&v, a->x, b->y, a->u, b->v, 1);
    put(&v, b->x, a->y, b->u, a->v, 1);
    put(&v, b->x, b->y, b->u, b->v, 1);
    put(&v, a->x, b->y, a->u, b->v, 1);
}

void render_vram_written(uint32_t addr, uint32_t size) {
    (void)addr; (void)size;     /* TODO: upload into a render target the region covers */
}

/* -- frames ------------------------------------------------------------------------------------------------------- */

static void end_frame(void) {
    vertex_used = batch_start = 0;
    staging_used = 0;
    last_valid = 0;
    frame_no++;
}

/* The end of a frame not shown (fast-forwarding): its work submitted, the frame's buffers and checks started again */
void render_finish_frame(void) {
    end_rendering();
    if (vk.recording)
        vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    end_frame();
}

void render_flip(uint32_t addr) {
    end_rendering();
    Target *t = target(addr, 0);
    if (!vk.surface || !t) {
        if (vk.recording)
            vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        end_frame();
        return;
    }
    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        vk_recreate_swapchain();
        end_frame();
        return;
    }
    VkCommandBuffer cmd = vk_cmd();
    vk_transition(&t->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii = {present_sampler, t->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, present_set, 0, 0, 1,
                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &w, 0, NULL);
    VkImg swap = {vk.swap_images[index], VK_NULL_HANDLE, vk.swap_views[index], vk.swap_format, vk.swap_extent.width,
                  vk.swap_extent.height, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT};
    vk_transition(&swap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, swap.view,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{{0, 0, 0, 1}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, vk.swap_extent}, 1, 0, 1, &ca, NULL,
                          NULL};
    vkCmdBeginRendering(cmd, &ri);
    /* letterboxed to 480:272 */
    float sw = (float)vk.swap_extent.width, sh = (float)vk.swap_extent.height;
    float vw = sw, vh = sw * 272.0f / 480.0f;
    if (vh > sh) {
        vh = sh;
        vw = sh * 480.0f / 272.0f;
    }
    VkViewport vp = {(sw - vw) / 2, (sh - vh) / 2, vw, vh, 0, 1};
    VkRect2D sc = {{0, 0}, vk.swap_extent};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &present_set, 0, NULL);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    float region[2] = {480.0f / RT_SIZE, 272.0f / RT_SIZE};
    vkCmdPushConstants(cmd, present_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof region, region);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    vk_transition(&swap, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vk_submit(vk.acquired, vk.rendered);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &vk.rendered, 1, &vk.swapchain, &index, NULL};
    r = vkQueuePresentKHR(vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        vk_recreate_swapchain();
    end_frame();
}

/* The displayed part of a framebuffer, for a frame dump: read back, and scaled down to 480 x 272 */
void render_readback(uint32_t addr, uint32_t *pixels) {
    end_rendering();
    Target *t = target(addr, 0);
    if (!t) {
        memset(pixels, 0, 480 * 272 * 4);
        return;
    }
    vk_transition(&t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uint32_t w = 480u * (uint32_t)scale, h = 272u * (uint32_t)scale;
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(vk_cmd(), t->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    const uint32_t *src = readback.map;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            pixels[y * 480 + x] = src[(uint32_t)(y * scale) * w + (uint32_t)(x * scale)] | 0xFF000000u;
}
