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
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ge.vert.inc"
#include "ge.frag.inc"
#include "present.vert.inc"
#include "present.frag.inc"
#include "shadow.vert.inc"
#include "shadow.frag.inc"
#include "pshadow.vert.inc"
#include "pshadow.frag.inc"
#include "ao.frag.inc"
#include "ao_blur.frag.inc"
#include "ao_apply.frag.inc"

int render_on;

#define RT_SIZE 512                 /* PSP pixels, square */
#define MAX_TARGETS 32
#define MAX_TEXTURES 4096
#define VERTEX_BYTES (64u << 20)
#define STAGING_BYTES (64u << 20)

typedef struct { float x, y, z, w, u, v; uint32_t color; float fog, wx, wy, wz; uint32_t sun; int16_t n[4]; } GpuVertex;

/* Lighting by the pixel: the sets of directional lights draws use (gpu.c gives them), in a storage buffer - half of it
 * each frame in flight - a draw naming its set */
#define LIGHT_SETS 8192
typedef struct { float dir[4][4], color[4][4]; uint32_t count, sun, pad[2]; } LightSet;
static VkBuf light_buf;
static uint32_t light_sets_used, prim_lights = ~0u;
static LightSet last_lights;

/* Point lights: the game has none. What it draws adding to the picture in the world (fire, glows) is a light: each such
 * draw's vertices give a place and a colour (with its texture's average colour), those near each other are one light,
 * and the warm ones light the next frame around them (MHFU_POINT_LIGHTS=0: none) */
#define MAX_EMITTERS 1024
#define MAX_POINT_LIGHTS 8
#define POINT_LIGHT_REACH 4000.0f       /* lights this far from what the camera looks at, at most */
#define POINT_LIGHT_RADIUS 900.0f       /* how far a light reaches */
#define POINT_LIGHT_GAIN 1.0f
static float point_gain = POINT_LIGHT_GAIN;     /* MHFU_POINT_GAIN: the lights' strength */
typedef struct { float pos[3], color[3], weight; } Emitter;
static Emitter emitters[MAX_EMITTERS];
static int nemitters, emitting, point_lights_on = -1;
static double emit_sum[7];          /* the draw being batched: weight, weighted position and colour */
static float tex_average[MAX_TEXTURES][3];
static uint8_t tex_cutout[MAX_TEXTURES];     /* the texture has holes (the alpha test's: leaves, grass) */
static LightSet point_lights[2];    /* the lights for the frame being made (two sets of four) */
static float view_center[3];        /* where the camera looks (the shadow map's centre), in the world */

/* Shadows: the sun's depth over the world around the camera's target (gpu.c places it: SHADOW_SIZE), drawn each frame
 * from what is drawn in the world - the same vertices, in a command buffer of its own submitted before the frame's */
#define SHADOW_SIZE 2048
static VkImg shadow_img;
static VkSampler shadow_sampler;
static VkPipeline shadow_pipeline;
static VkCommandBuffer shadow_cmd, shadow_cmds[2];
static int shadow_recording, shadow_on, prim_world;
static float light_vp[16];
/* The point lights' shadows: the two strongest lights' cube maps (6 faces each, distance from the light over its
 * reach), drawn in one pass from what casts shadows - each draw once into all twelve faces (instances; the vertex
 * shader picks the face's layer) - in a command buffer of their own before the frame's */
#define POINT_SHADOW_SIZE 512
static VkImg point_img;
static VkImageView point_cube[2];
static VkSampler point_sampler;
static VkPipeline point_pipeline;
static VkCommandBuffer point_cmd, point_cmds[2];
static int point_recording, point_shadows, point_shadows_next;
static int point_shadows_on = 1;    /* MHFU_POINT_SHADOWS=0: none */
/* What clipping at the camera's near plane cut off (behind the camera, it still casts shadows): whole, for the shadow
 * pass only, in a buffer of their own (one a frame in flight) */
#define SHADOW_EXTRA_BYTES (8u << 20)
static VkBuf shadow_extra[2], extra;
static VkDeviceSize extra_used, extra_batch;
/* The scenery's shading by the sun: it comes lit beforehand, without normals - they are made from its triangles, each
 * batch's (MHFU_SUN_SHADING=0: none): the batch's places in the world and on the screen, kept as they are put */
static int sun_shading = -1;
typedef struct { float w[3], sx, sy; } Corner;
static Corner *corners;
static size_t corners_cap;
/* Ambient occlusion: from the depth of the world once it is drawn (before what is drawn over it - the HUD), at half
 * resolution, blurred, multiplied into the picture (MHFU_AO=0: none; MHFU_AO_STRENGTH, MHFU_AO_RADIUS) */
#define AO_FADE 6000.0f                 /* gone by this far */
static int ao_on = -1;
static float ao_strength = 2.0f, ao_radius = 120.0f;
static VkImg ao_img[2];
static VkPipeline ao_pipeline, ao_blur_pipeline, ao_apply_pipeline;
static VkPipelineLayout ao_layout;
static VkDescriptorSetLayout ao_set_layout;
static VkDescriptorPool ao_pool;
#define AO_PER_FRAME 8                  /* times a frame (the frame dumps draw it again) */
static VkDescriptorSet ao_sets[2][AO_PER_FRAME][4];     /* a frame in flight's: the pass, the two blurs, the picture */
static VkSampler depth_sampler;
static float prim_view[16], ao_view[16];    /* the camera's space to the screen: the primitive's, the world's */
static float prim_world_view[12], ao_world_view[12];    /* and to the world (3 x 4, by columns) */
static struct Target *ao_target;        /* the solid world was drawn into it, without occlusion yet */
static struct Target *ao_applied;       /* it has its occlusion (until it is cleared) */
static int ao_used;                     /* times this frame */
/* its passes' push constants (ao_common.glsl) */
typedef struct { float to_view[16], params[4], params2[4], area[4], lights[4][4], colors[4][4]; } AoPush;

typedef struct Target {
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
    VkCullModeFlags cull;
    VkFrontFace front;
    uint32_t lights;                /* its light set (by the pixel), ~0: none */
} DrawState;

/* Whether a draw is shaded by the sun: the solid scenery (not what has holes in its texture - leaves, grass, whose
 * cards' normals mean nothing - nor what writes no depth or adds to the picture) */
static int shaded(const DrawState *d) {
    return (d->flags & 1792u) == 256u && d->depth_write && sun_shading && shadow_on &&
           (d->tex_slot >= MAX_TEXTURES || !tex_cutout[d->tex_slot]) &&
           (!d->blend || (d->eq.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
                          d->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA));
}

static int scale = 2;
static Target targets[MAX_TARGETS];
static Texture *textures;
static int ntextures;
static VkBuf vertices, staging;    /* the frame's (one of two: vk.frame) */
static VkBuf frame_vertices[2], frame_staging[2];
/* images replaced while the GPU may still draw with them: freed when their frame comes round again */
static VkImg retired[2][64];
static int nretired[2];
static VkDeviceSize vertex_used, staging_used;
static VkPipeline pipeline, present_pipeline;
static VkPipelineLayout layout, present_layout;
static VkDescriptorSetLayout set_layout, present_set_layout;
static VkDescriptorPool pool;
static VkDescriptorSet set, present_sets[2];
static VkSampler samplers[8], present_sampler;
static unsigned long frame_no = 1;

static Target *rendering;            /* the target being rendered into, or NULL */
static DrawState cur;                /* the current primitive's state */
static DrawState last;               /* the last draw's, as set in the command buffer */
static int last_valid;
static VkDeviceSize batch_start;     /* vertices of the batch not drawn yet */
static VkBuf readback;

static int shadow_finish(VkCommandBuffer *out);
static void frame_reused(int frame);

/* -- setup -------------------------------------------------------------------------------------------------------- */

static VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout pl, VkFormat color,
                                VkFormat depth, int ge) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
    };
    VkVertexInputBindingDescription bind = {0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[7] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24},
        {3, 0, VK_FORMAT_R32_SFLOAT, 28},
        {4, 0, VK_FORMAT_R32G32B32_SFLOAT, 32},
        {5, 0, VK_FORMAT_R8G8B8A8_UNORM, 44},
        {6, 0, VK_FORMAT_R16G16B16A16_SNORM, 48},
    };
    int verts = ge >= 1 && ge <= 3;
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
                                               verts ? 1u : 0u, &bind, verts ? 7u : 0u, attrs};
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
        VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE};
    VkDynamicState dyn_present[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0,
                                           verts ? 11u : 2u, verts ? dyn_ge : dyn_present};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &color, depth,
                                        VK_FORMAT_UNDEFINED};
    if (ge == 4) {                      /* multiplied into what is there (the colour, not its alpha) */
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
        att.colorWriteMask = 7;
    }
    if (ge == 3) {                      /* the point lights' (depth: the distance, written by the shader): no bias */
        ri.colorAttachmentCount = 0;
        cb.attachmentCount = 0;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn_present;
    }
    if (ge == 2) {                      /* the shadow map's: depth only, its faces both ways, a slope-scaled bias */
        ri.colorAttachmentCount = 0;
        cb.attachmentCount = 0;
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasSlopeFactor = 1.5f;
        rs.depthBiasConstantFactor = 0.0f;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn_present;
    }
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vi, &ia,
                                        NULL, &vp, &rs, &ms, verts ? &ds : NULL, &cb, &dy, pl, VK_NULL_HANDLE, 0,
                                        VK_NULL_HANDLE, -1};
    VkPipeline p;
    if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p) != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: pipeline creation failed\n");
        exit(1);
    }
    return p;
}

/* window.c: the internal resolution to use unless MHFU_SCALE says (enough for the screen's lines) */
void render_default_scale(int s) {
    if (s >= 1 && s <= 8)
        scale = s;
}

/* Ambient occlusion's images, pipelines and descriptor sets (each pass's: its input, and the depth) */
static void ao_init(void) {
    ao_on = !getenv("MHFU_AO") || atoi(getenv("MHFU_AO")) != 0;
    if (getenv("MHFU_AO_STRENGTH"))
        ao_strength = (float)atof(getenv("MHFU_AO_STRENGTH"));
    if (getenv("MHFU_AO_RADIUS"))
        ao_radius = (float)atof(getenv("MHFU_AO_RADIUS"));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.gpu, &props);
    if (props.limits.maxPushConstantsSize < sizeof(AoPush))
        ao_on = 0;
    if (!ao_on)
        return;
    for (int i = 0; i < 2; i++)
        ao_img[i] = vk_image(240u * (uint32_t)scale, 136u * (uint32_t)scale, VK_FORMAT_R16G16_SFLOAT,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    vkCreateDescriptorSetLayout(vk.device, &dl, NULL, &ao_set_layout);
    enum { SETS = 2 * AO_PER_FRAME * 4 };
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * SETS};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SETS, 1, &ps};
    vkCreateDescriptorPool(vk.device, &dpi, NULL, &ao_pool);
    VkDescriptorSetLayout layouts[SETS];
    for (int i = 0; i < SETS; i++)
        layouts[i] = ao_set_layout;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, ao_pool, SETS, layouts};
    vkAllocateDescriptorSets(vk.device, &dai, &ao_sets[0][0][0]);
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(AoPush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &ao_set_layout, 1, &pr};
    vkCreatePipelineLayout(vk.device, &pli, NULL, &ao_layout);
    VkShaderModule vs = vk_shader(present_vert, sizeof present_vert);
    ao_pipeline = make_pipeline(vs, vk_shader(ao_frag, sizeof ao_frag), ao_layout, VK_FORMAT_R16G16_SFLOAT,
                                VK_FORMAT_UNDEFINED, 0);
    ao_blur_pipeline = make_pipeline(vs, vk_shader(ao_blur_frag, sizeof ao_blur_frag), ao_layout,
                                     VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED, 0);
    ao_apply_pipeline = make_pipeline(vs, vk_shader(ao_apply_frag, sizeof ao_apply_frag), ao_layout,
                                      VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, 4);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk.device, &sci, NULL, &depth_sampler);
}

int render_init(void *sdl_window) {
    const char *s = getenv("MHFU_SCALE");
    if (s && atoi(s) >= 1 && atoi(s) <= 8)
        scale = atoi(s);
    if (!vk_init(sdl_window))
        return 0;

    /* bindless textures: one array, filled as textures are made */
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorBindingFlags bf[4] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                      VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                                      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                                      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                                      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 4, bf};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi,
                                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 4, b};
    vkCreateDescriptorSetLayout(vk.device, &dl, NULL, &set_layout);
    VkDescriptorSetLayoutBinding pb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       NULL};
    VkDescriptorSetLayoutCreateInfo pdl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &pb};
    vkCreateDescriptorSetLayout(vk.device, &pdl, NULL, &present_set_layout);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES + 5},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                      VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 3, 2, ps};
    vkCreateDescriptorPool(vk.device, &dpi, NULL, &pool);
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &set_layout};
    vkAllocateDescriptorSets(vk.device, &dai, &set);
    dai.pSetLayouts = &present_set_layout;
    vkAllocateDescriptorSets(vk.device, &dai, &present_sets[0]);
    vkAllocateDescriptorSets(vk.device, &dai, &present_sets[1]);

    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout, 1, &pr};
    vkCreatePipelineLayout(vk.device, &pli, NULL, &layout);
    VkPushConstantRange ppr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo ppli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &present_set_layout,
                                       1, &ppr};
    vkCreatePipelineLayout(vk.device, &ppli, NULL, &present_layout);

    VkShaderModule vs = vk_shader(ge_vert, sizeof ge_vert), fs = vk_shader(ge_frag, sizeof ge_frag);
    pipeline = make_pipeline(vs, fs, layout, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 1);
    shadow_pipeline = make_pipeline(vk_shader(shadow_vert, sizeof shadow_vert), vk_shader(shadow_frag, sizeof shadow_frag),
                                    layout, VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, 2);
    if (vk.output_layer)
        point_pipeline = make_pipeline(vk_shader(pshadow_vert, sizeof pshadow_vert),
                                       vk_shader(pshadow_frag, sizeof pshadow_frag), layout, VK_FORMAT_UNDEFINED,
                                       VK_FORMAT_D32_SFLOAT, 3);
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
    VkSamplerCreateInfo shi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    shi.magFilter = shi.minFilter = VK_FILTER_LINEAR;
    shi.addressModeU = shi.addressModeV = shi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shi.compareEnable = VK_TRUE;
    shi.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(vk.device, &shi, NULL, &shadow_sampler);
    VkSamplerCreateInfo psi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    psi.magFilter = psi.minFilter = VK_FILTER_LINEAR;
    psi.addressModeU = psi.addressModeV = psi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk.device, &psi, NULL, &present_sampler);

    for (int i = 0; i < 2; i++) {
        frame_vertices[i] = vk_buffer(VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
        frame_staging[i] = vk_buffer(STAGING_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
    }
    vertices = frame_vertices[0];
    staging = frame_staging[0];
    for (int i = 0; i < 2; i++)
        shadow_extra[i] = vk_buffer(SHADOW_EXTRA_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
    extra = shadow_extra[0];
    readback = vk_buffer((VkDeviceSize)RT_SIZE * scale * RT_SIZE * scale * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    textures = calloc(MAX_TEXTURES, sizeof *textures);
    shadow_img = vk_image(SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkDescriptorImageInfo sii = {shadow_sampler, shadow_img.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet sw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sii, NULL, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &sw, 0, NULL);
    point_img = vk_image_layers(POINT_SHADOW_SIZE, POINT_SHADOW_SIZE, 12, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkSamplerCreateInfo psci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    psci.magFilter = psci.minFilter = VK_FILTER_NEAREST;
    psci.addressModeU = psci.addressModeV = psci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk.device, &psci, NULL, &point_sampler);
    VkDescriptorImageInfo pii[2];
    for (int i = 0; i < 2; i++) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, point_img.image,
                                     VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_D32_SFLOAT, {0},
                                     {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, (uint32_t)(6 * i), 6}};
        vkCreateImageView(vk.device, &vci, NULL, &point_cube[i]);
        pii[i] = (VkDescriptorImageInfo){point_sampler, point_cube[i], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet pw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 3, 0, 2,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pii, NULL, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &pw, 0, NULL);
    light_buf = vk_buffer(2 * LIGHT_SETS * sizeof(LightSet), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
    VkDescriptorBufferInfo lbi = {light_buf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet lw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &lbi, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &lw, 0, NULL);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, vk.pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    vkAllocateCommandBuffers(vk.device, &cai, shadow_cmds);
    shadow_cmd = shadow_cmds[0];
    vkAllocateCommandBuffers(vk.device, &cai, point_cmds);
    point_cmd = point_cmds[0];
    ao_init();
    vk.before_submit = shadow_finish;
    vk.frame_reused = frame_reused;
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
            t->depth = vk_image(px, px, VK_FORMAT_D32_SFLOAT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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

/* An image the GPU may still be drawing with: freed once the frame comes round again */
static void retire(VkImg *i) {
    if (!i->image)
        return;
    if (nretired[vk.frame] < 64)
        retired[vk.frame][nretired[vk.frame]++] = *i;
    else
        vk_image_free(i);          /* (never: a frame does not replace that many) */
    memset(i, 0, sizeof *i);
}

/* vk_submit: the frame's resources are free again */
static void frame_reused(int frame) {
    vertices = frame_vertices[frame];
    staging = frame_staging[frame];
    extra = shadow_extra[frame];
    shadow_cmd = shadow_cmds[frame];
    vkResetCommandBuffer(shadow_cmd, 0);
    point_cmd = point_cmds[frame];
    vkResetCommandBuffer(point_cmd, 0);
    for (int i = 0; i < nretired[frame]; i++)
        vk_image_free(&retired[frame][i]);
    nretired[frame] = 0;
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
            retire(&t->img);
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
    if (t->slot < MAX_TEXTURES) {      /* its average colour (by alpha), for the lights effects make */
        const uint32_t *px = (const uint32_t *)((uint8_t *)staging.map + staging_used);
        double sum[3] = {0, 0, 0}, wsum = 0;
        int step = w * h > 4096 ? (w * h) / 4096 : 1, n = 0, holes = 0;
        for (int i = 0; i < w * h; i += step) {
            uint32_t c = px[i];
            double a = (double)(c >> 24) / 255.0;
            n++;
            holes += (c >> 24) < 128;
            sum[0] += a * (c & 255);
            sum[1] += a * ((c >> 8) & 255);
            sum[2] += a * ((c >> 16) & 255);
            wsum += a;
        }
        for (int k = 0; k < 3; k++)
            tex_average[t->slot][k] = wsum > 0 ? (float)(sum[k] / wsum / 255.0) : 0;
        tex_cutout[t->slot] = holes * 50 > n;
    }
    if (t->w != w || t->h != h) {
        retire(&t->img);
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

static void flush_emitter(void);

/* -- ambient occlusion -------------------------------------------------------------------------------------------- */

/* gpu.c: the next primitive's camera space to the screen and to the world (if it is in the world) */
void render_prim_view(const float *screen_from_view, const float *world_from_view) {
    memcpy(prim_view, screen_from_view, sizeof prim_view);
    memcpy(prim_world_view, world_from_view, sizeof prim_world_view);
}

static int invert4(const float *m, float *out) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++)
            a[r][c] = c < 4 ? m[4 * c + r] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int best = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[best][c]))
                best = r;
        if (fabs(a[best][c]) < 1e-12)
            return 0;
        for (int k = 0; k < 8; k++) {
            double t = a[c][k];
            a[c][k] = a[best][k];
            a[best][k] = t;
        }
        double d = a[c][c];
        for (int k = 0; k < 8; k++)
            a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = a[r][c];
                for (int k = 0; k < 8; k++)
                    a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[4 * c + r] = (float)a[r][4 + c];
    return 1;
}

static void ao_set(VkDescriptorSet ds, VkImageView source, VkImageLayout layout, VkSampler sampler, VkImageView depth) {
    VkDescriptorImageInfo ii[2] = {{sampler, source, layout},
                                   {depth_sampler, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0],
         NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[1],
         NULL, NULL}};
    vkUpdateDescriptorSets(vk.device, 2, w, 0, NULL);
}

/* One of its passes: a triangle over w x h of an image */
static void ao_draw(VkImageView view, uint32_t w, uint32_t h, VkPipeline p, VkDescriptorSet ds, const AoPush *pc,
                    int load) {
    VkCommandBuffer cmd = vk_cmd();
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, 1, 0, 1, &ca, NULL, NULL};
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, ao_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *pc, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

/* The world is drawn into t: its occlusion, multiplied in */
static void ao_apply(Target *t) {
    ao_target = NULL;
    ao_applied = t;
    if (ao_on <= 0 || ao_used >= AO_PER_FRAME)
        return;
    AoPush pc;
    if (!invert4(ao_view, pc.to_view))
        return;
    end_rendering();
    uint32_t fw = 480u * (uint32_t)scale, fh = 272u * (uint32_t)scale, hw = fw / 2, hh = fh / 2;
    float focal = fabsf(ao_view[0]) * (float)scale;
    memcpy(pc.params, (float[4]){ao_radius, ao_strength, (float)scale, focal}, sizeof pc.params);
    memcpy(pc.params2, (float[4]){AO_FADE, 0, 0, 0}, sizeof pc.params2);
    memcpy(pc.area, (float[4]){(float)fw, (float)fh, (float)hw, (float)hh}, sizeof pc.area);
    /* the point lights (the first four), in the camera's space: where they shine, the occlusion takes nothing - it
     * takes away the light from all round, not theirs */
    memset(pc.lights, 0, sizeof pc.lights + sizeof pc.colors);
    float m4[16] = {0}, inv[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 3; i++)
            m4[4 * c + i] = ao_world_view[3 * c + i];
    m4[15] = 1;
    if (point_lights_on > 0 && invert4(m4, inv))
        for (uint32_t i = 0; i < point_lights[0].count && i < 4; i++) {
            const float *w = point_lights[0].dir[i];
            for (int k = 0; k < 3; k++)
                pc.lights[i][k] = inv[k] * w[0] + inv[4 + k] * w[1] + inv[8 + k] * w[2] + inv[12 + k];
            pc.lights[i][3] = w[3];
            memcpy(pc.colors[i], point_lights[0].color[i], sizeof pc.colors[i]);
        }
    VkDescriptorSet *sets = ao_sets[vk.frame][ao_used++];
    VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ao_set(sets[0], t->depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, depth_sampler, t->depth.view);
    ao_set(sets[1], ao_img[0].view, ro, depth_sampler, t->depth.view);
    ao_set(sets[2], ao_img[1].view, ro, depth_sampler, t->depth.view);
    ao_set(sets[3], ao_img[0].view, ro, depth_sampler, t->depth.view);
    vk_transition(&t->depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    vk_transition(&ao_img[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ao_draw(ao_img[0].view, hw, hh, ao_pipeline, sets[0], &pc, 0);
    for (int pass = 0; pass < 2; pass++) {     /* blurred across, then down: back into the first */
        VkImg *src = &ao_img[pass], *dst = &ao_img[1 - pass];
        vk_transition(src, ro);
        vk_transition(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        pc.params2[1] = pass == 0 ? 1.0f : 0.0f;
        pc.params2[2] = pass == 0 ? 0.0f : 1.0f;
        ao_draw(dst->view, hw, hh, ao_blur_pipeline, sets[1 + pass], &pc, 0);
    }
    vk_transition(&ao_img[0], ro);
    vk_transition(&t->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ao_draw(t->color.view, fw, fh, ao_apply_pipeline, sets[3], &pc, 1);
}

void render_prim_begin(const uint32_t *reg) {
    if (emitting)
        flush_emitter();            /* the last primitive's light (each its own, batched or not) */
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
    s.cull = VK_CULL_MODE_NONE;
    s.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    s.lights = ~0u;
    if (prim_world && !clear) {
        s.flags |= 256u | ((reg[0x17] & 1) ? (prim_lights != ~0u ? 1024u : 512u) : 0u);
        s.lights = prim_lights;
        if (reg[0x1D] & 1) {            /* the culling gpu.c leaves to the GPU (the same faces as it would drop) */
            s.cull = VK_CULL_MODE_BACK_BIT;
            s.front = (reg[0x9B] & 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        }
    }
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
    /* the solid world drawn, then what is not solid (glows, fire, smoke, the HUD): the world's occlusion first - only
     * the solid world's colours have it, once (until the picture is cleared) */
    int solid = (s.flags & 256u) && s.depth_write;
    if (ao_target && s.target == ao_target && !solid) {
        flush();
        ao_apply(ao_target);
    }
    if (clear && s.target == ao_applied)
        ao_applied = NULL;
    if (solid && s.target != ao_applied) {
        ao_target = s.target;
        memcpy(ao_view, prim_view, sizeof ao_view);
        memcpy(ao_world_view, prim_world_view, sizeof ao_world_view);
    }
    if (memcmp(&s, &cur, sizeof s)) {
        flush();
        cur = s;
    }
    if (point_lights_on < 0) {
        point_lights_on = !getenv("MHFU_POINT_LIGHTS") || atoi(getenv("MHFU_POINT_LIGHTS")) != 0;
        point_shadows_on = !getenv("MHFU_POINT_SHADOWS") || atoi(getenv("MHFU_POINT_SHADOWS")) != 0;
        sun_shading = !getenv("MHFU_SUN_SHADING") || atoi(getenv("MHFU_SUN_SHADING")) != 0;
        if (getenv("MHFU_POINT_GAIN"))
            point_gain = (float)atof(getenv("MHFU_POINT_GAIN"));
    }
    emitting = point_lights_on && (s.flags & 256u) && s.blend && s.eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE &&
               s.eq.colorBlendOp == VK_BLEND_OP_ADD;
}

typedef struct {
    float w, h;
    uint32_t slot, flags, alpha, env, fog, pad;
    float light[16];
    uint32_t points, pad2[3];       /* the point lights' two sets (or ~0: none) */
} PushConstants;

/* The shadow map's pass, begun at the first thing in the world of a frame: cleared to the farthest */
static void shadow_begin(void) {
    if (shadow_recording)
        return;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(shadow_cmd, &bi);
    shadow_recording = 1;
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, shadow_img.image, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(shadow_cmd, &d);
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, shadow_img.view,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}}, 1, 0, 0, NULL,
                          &da, NULL};
    vkCmdBeginRendering(shadow_cmd, &ri);
    vkCmdBindPipeline(shadow_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
    vkCmdBindDescriptorSets(shadow_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(shadow_cmd, 0, 1, &vertices.buffer, &zero);
    VkViewport vp = {0, 0, SHADOW_SIZE, SHADOW_SIZE, 0, 1};
    VkRect2D sc = {{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}};
    vkCmdSetViewport(shadow_cmd, 0, 1, &vp);
    vkCmdSetScissor(shadow_cmd, 0, 1, &sc);
}

/* The sun's shadow pass, finished (NULL if there was none) */
static VkCommandBuffer sun_finish(void) {
    if (!shadow_recording)
        return VK_NULL_HANDLE;
    vkCmdEndRendering(shadow_cmd);
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, shadow_img.image, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(shadow_cmd, &d);
    vkEndCommandBuffer(shadow_cmd);
    shadow_recording = 0;
    return shadow_cmd;
}

/* The point lights' shadow pass, begun at the first thing in the world of a frame with shadowed point lights */
static void point_begin(void) {
    if (point_recording)
        return;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(point_cmd, &bi);
    point_recording = 1;
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, point_img.image, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 12}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(point_cmd, &d);
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, point_img.view,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {POINT_SHADOW_SIZE, POINT_SHADOW_SIZE}},
                          12, 0, 0, NULL, &da, NULL};
    vkCmdBeginRendering(point_cmd, &ri);
    vkCmdBindPipeline(point_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, point_pipeline);
    vkCmdBindDescriptorSets(point_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(point_cmd, 0, 1, &vertices.buffer, &zero);
    VkViewport vp = {0, 0, POINT_SHADOW_SIZE, POINT_SHADOW_SIZE, 0, 1};
    VkRect2D sc = {{0, 0}, {POINT_SHADOW_SIZE, POINT_SHADOW_SIZE}};
    vkCmdSetViewport(point_cmd, 0, 1, &vp);
    vkCmdSetScissor(point_cmd, 0, 1, &sc);
}

static VkCommandBuffer point_finish(void) {
    if (!point_recording)
        return VK_NULL_HANDLE;
    vkCmdEndRendering(point_cmd);
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, point_img.image, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 12}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(point_cmd, &d);
    vkEndCommandBuffer(point_cmd);
    point_recording = 0;
    return point_cmd;
}

/* vk_submit: the shadow passes, finished, to submit before the frame's commands */
static int shadow_finish(VkCommandBuffer *out) {
    int n = 0;
    VkCommandBuffer c = sun_finish();
    if (c)
        out[n++] = c;
    c = point_finish();
    if (c)
        out[n++] = c;
    return n;
}

/* gpu.c: this frame's shadows - the world to the shadow map (by columns), or off */
float rt_cull_margin;

void render_shadow_frame(const float *m, int on) {
    /* with shadows, what is just out of view is drawn too (MHFU_CULL_MARGIN: how far, 1200) */
    static float margin = -1;
    if (margin < 0)
        margin = getenv("MHFU_CULL_MARGIN") ? (float)atof(getenv("MHFU_CULL_MARGIN")) : 1200.0f;
    rt_cull_margin = on ? margin : 0.0f;
    if (on && memcmp(m, light_vp, sizeof light_vp)) {
        flush();                        /* what is batched was placed with the last */
        memcpy(light_vp, m, sizeof light_vp);
        /* the map's centre: the point at x = y = 0, depth 0.5 (the light's rows are orthonormal, scaled) */
        float sx = 1 / (m[0] * m[0] + m[4] * m[4] + m[8] * m[8]), sy = 1 / (m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
        float sz = 1 / (m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
        for (int i = 0; i < 3; i++)
            view_center[i] = -(m[4 * i] * m[12] * sx + m[4 * i + 1] * m[13] * sy + m[4 * i + 2] * (m[14] - 0.5f) * sz);
    }
    if (on != shadow_on)
        flush();
    shadow_on = on;
}

/* gpu.c: the next primitive's directional lights, for the GPU to light it by the pixel (n 0: lit by the vertex) - in
 * the world: direction (to the light; w a power of the diffuse term, 0 none) and colour; which is the sun */
void render_prim_lights(int n, const float *dirs, const float *colors, int sun) {
    if (!n) {
        prim_lights = ~0u;
        return;
    }
    LightSet l;
    memset(&l, 0, sizeof l);
    memcpy(l.dir, dirs, sizeof(float) * 4 * (size_t)n);
    memcpy(l.color, colors, sizeof(float) * 4 * (size_t)n);
    l.count = (uint32_t)n;
    l.sun = sun < 0 ? 99u : (uint32_t)sun;
    if (prim_lights != ~0u && light_sets_used && !memcmp(&l, &last_lights, sizeof l))
        return;
    if (light_sets_used >= LIGHT_SETS - 2) {     /* (the last two: the point lights) */
        prim_lights = ~0u;
        return;
    }
    uint32_t index = (uint32_t)vk.frame * LIGHT_SETS + light_sets_used++;
    memcpy((LightSet *)light_buf.map + index, &l, sizeof l);
    last_lights = l;
    prim_lights = index;
}

/* gpu.c: the next primitive's vertices have their place in the world (shadows fall on it, and it casts them) */
void render_prim_world(int on) {
    prim_world = on;
}

/* The batch's light, if it adds to the picture: an emitter, its colour the vertices' times the texture's */
static void flush_emitter(void) {
    if (emit_sum[0] > 0 && nemitters < MAX_EMITTERS) {
        Emitter *e = &emitters[nemitters++];
        double w = emit_sum[0];
        for (int k = 0; k < 3; k++) {
            e->pos[k] = (float)(emit_sum[1 + k] / w);
            e->color[k] = (float)(emit_sum[4 + k] / w);
            if (cur.tex_slot < MAX_TEXTURES)
                e->color[k] *= tex_average[cur.tex_slot][k];
        }
        e->weight = (float)w;
    }
    memset(emit_sum, 0, sizeof emit_sum);
}

/* How much a light matters: how bright it is, less the further it is from what the camera looks at */
static float light_rank(const float *pos, float power) {
    float d = sqrtf((pos[0] - view_center[0]) * (pos[0] - view_center[0]) + (pos[1] - view_center[1]) * (pos[1] - view_center[1]) +
                    (pos[2] - view_center[2]) * (pos[2] - view_center[2]));
    return power / (1.0f + d / 1000.0f);
}

/* The frame's emitters to lights: those within 300 units one, the warm (fire) ones kept, the strongest first */
static void make_point_lights(void) {
    typedef struct { float pos[3], color[3], weight; } Light;
    Light l[64];
    int n = 0;
    for (int i = 0; i < nemitters; i++) {
        Emitter *e = &emitters[i];
        int j = 0;
        for (; j < n; j++) {
            float d2 = 0;
            for (int k = 0; k < 3; k++)
                d2 += (l[j].pos[k] - e->pos[k]) * (l[j].pos[k] - e->pos[k]);
            if (d2 < 300.0f * 300.0f)
                break;
        }
        if (j == n) {
            if (n == 64)
                continue;
            memset(&l[n], 0, sizeof l[n]);
            n++;
        }
        float w0 = l[j].weight, w = w0 + e->weight;
        for (int k = 0; k < 3; k++) {
            l[j].pos[k] = (l[j].pos[k] * w0 + e->pos[k] * e->weight) / w;
            l[j].color[k] = (l[j].color[k] * w0 + e->color[k] * e->weight) / w;
        }
        l[j].weight = w;
    }
    /* the warm ones (fire: red over green over blue) near what the camera looks at, smoothed from frame to frame -
     * a light found again near where one was eases to its new colour and strength, one gone fades out */
    typedef struct { float pos[3], color[3], strength, power; int seen; } Smoothed;
    static Smoothed lit[MAX_POINT_LIGHTS];
    for (int i = 0; i < MAX_POINT_LIGHTS; i++)
        lit[i].seen = 0;
    for (int j = 0; j < n; j++) {
        const float *c = l[j].color;
        float dc = sqrtf((l[j].pos[0] - view_center[0]) * (l[j].pos[0] - view_center[0]) +
                         (l[j].pos[1] - view_center[1]) * (l[j].pos[1] - view_center[1]) +
                         (l[j].pos[2] - view_center[2]) * (l[j].pos[2] - view_center[2]));
        /* by its hue: a fire's textures can be mostly dark, their average dark (0.14, 0.11, 0.06 in the guild hall) */
        if (dc > POINT_LIGHT_REACH || !(c[0] > 0.04f && c[0] >= c[1] && c[1] >= c[2] && c[0] > 1.4f * c[2]))
            continue;
        float strength = fminf(1.0f, l[j].weight / 40.0f);
        float m = fmaxf(c[0], 1e-3f);
        int k = -1;
        for (int i = 0; i < MAX_POINT_LIGHTS && k < 0; i++) {
            float d2 = 0;
            for (int q = 0; q < 3; q++)
                d2 += (lit[i].pos[q] - l[j].pos[q]) * (lit[i].pos[q] - l[j].pos[q]);
            if (lit[i].strength > 0 && d2 < 250.0f * 250.0f)
                k = i;
        }
        if (k < 0) {                        /* a new light: in an empty place, or instead of the weakest */
            k = 0;
            for (int i = 1; i < MAX_POINT_LIGHTS; i++)
                if (lit[i].strength < lit[k].strength)
                    k = i;
            if (lit[k].strength > strength)
                continue;
            memcpy(lit[k].pos, l[j].pos, sizeof lit[k].pos);
            for (int q = 0; q < 3; q++)
                lit[k].color[q] = c[q] / m;
            lit[k].strength = 0;
            lit[k].power = l[j].weight;
        }
        /* a fire stands still: what is drawn of its flames moves about (80 units) as they play and the camera
         * turns, so its light drifts to it only slowly (a third of a second); it brightens quickly and dims slowly */
        for (int q = 0; q < 3; q++) {
            lit[k].pos[q] += (l[j].pos[q] - lit[k].pos[q]) * 0.005f;
            lit[k].color[q] += (c[q] / m - lit[k].color[q]) * 0.05f;
        }

        /* (uncapped: which have shadows) - the brightest it has been seen lately: how much of a fire's flames is
         * drawn changes as the camera turns, the fire does not */
        lit[k].power = fmaxf(lit[k].power * 0.9995f, l[j].weight);
        /* its strength by that too (a fire turned from is no dimmer), reached in a fraction of a second */
        float target = fminf(1.0f, lit[k].power / 40.0f);
        lit[k].strength += (target - lit[k].strength) * 0.1f;
        lit[k].seen = 1;
    }
    memset(point_lights, 0, sizeof point_lights);
    for (int i = 0; i < MAX_POINT_LIGHTS; i++)
        if (!lit[i].seen)
            lit[i].strength *= 0.9995f;     /* (half in ten seconds: out of view the game draws no flames) */
    /* the brightest first (as they give light, not capped), the nearer the more. The first two (of the first set)
     * have shadows, and keep them - and their cube maps - until another is half as bright again */
    static int shadowed[2] = {-1, -1};
    float rank[MAX_POINT_LIGHTS];
    for (int i = 0; i < MAX_POINT_LIGHTS; i++)
        rank[i] = lit[i].strength > 0.02f ? light_rank(lit[i].pos, lit[i].power) : 0.0f;
    for (int slot = 0; slot < 2; slot++) {
        int best = -1;
        for (int i = 0; i < MAX_POINT_LIGHTS; i++)
            if (rank[i] > 0 && i != shadowed[1 - slot] && (best < 0 || rank[i] > rank[best]))
                best = i;
        int cur_i = shadowed[slot];
        if (cur_i < 0 || rank[cur_i] <= 0 || (best >= 0 && best != cur_i && rank[best] > 1.5f * rank[cur_i]))
            shadowed[slot] = best;
    }
    if (shadowed[0] < 0 && shadowed[1] >= 0) {
        shadowed[0] = shadowed[1];
        shadowed[1] = -1;
    }
    int order[MAX_POINT_LIGHTS], no = 0;
    for (int slot = 0; slot < 2; slot++)
        if (shadowed[slot] >= 0)
            order[no++] = shadowed[slot];
    for (int i = 0; i < MAX_POINT_LIGHTS; i++)
        if (i != shadowed[0] && i != shadowed[1])
            order[no++] = i;
    for (int i = (shadowed[0] >= 0) + (shadowed[1] >= 0) + 1; i < MAX_POINT_LIGHTS; i++)     /* the rest by rank */
        for (int j = i; j > (shadowed[0] >= 0) + (shadowed[1] >= 0) && rank[order[j]] > rank[order[j - 1]]; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (int oi = 0; oi < MAX_POINT_LIGHTS; oi++) {
        int i = order[oi];
        if (lit[i].strength < 0.02f) {
            lit[i].strength = 0;
            continue;
        }
        LightSet *ls = &point_lights[point_lights[0].count < 4 ? 0 : 1];
        uint32_t at = ls->count++;
        memcpy(ls->dir[at], lit[i].pos, sizeof lit[i].pos);
        ls->dir[at][1] += 70.0f;            /* a fire's light: in its flames, above where they are drawn on average */
        ls->dir[at][3] = POINT_LIGHT_RADIUS;
        /* a fire's light flickers: two slow waves, a light's own (by where it is) */
        double now = (double)time_us() / 1e6, phase = lit[i].pos[0] * 0.013 + lit[i].pos[2] * 0.017;
        float flicker = 0.94f + 0.04f * (float)sin(now * 9.1 + phase) + 0.02f * (float)sin(now * 23.7 + 2 * phase);
        for (int q = 0; q < 3; q++)
            ls->color[at][q] = lit[i].color[q] * lit[i].strength * point_gain * flicker;
    }
    point_shadows_next = (shadowed[0] >= 0 && lit[shadowed[0]].strength >= 0.02f) +
                         (shadowed[1] >= 0 && lit[shadowed[1]].strength >= 0.02f);
    if (point_shadows_next > (int)point_lights[0].count)
        point_shadows_next = (int)point_lights[0].count;
    if (!vk.output_layer || !shadow_on || !point_shadows_on)
        point_shadows_next = 0;
    point_lights[0].pad[0] = (uint32_t)point_shadows_next;
    nemitters = 0;
}

/* Draw what has been batched with the current state */
/* The batch's normals (the scenery's): each triangle's, turned towards the camera, summed where corners meet (the same
 * place in the world), each corner taking the sum unless it bends more than 60 degrees from its own (an edge) */
static void make_normals(size_t n) {
    static uint32_t *table, *owner;     /* a place's first corner + 1 (0: empty); each corner's first at its place */
    static float (*sum)[3], (*face)[3];
    static size_t table_cap, sum_cap;
    if (n > sum_cap) {
        sum_cap = n * 2;
        sum = realloc(sum, sum_cap * sizeof *sum);
        face = realloc(face, sum_cap * sizeof *face);
        owner = realloc(owner, sum_cap * sizeof *owner);
    }
    size_t tsize = 1024;
    while (tsize < n * 2)
        tsize *= 2;
    if (tsize > table_cap) {
        table_cap = tsize;
        table = realloc(table, table_cap * sizeof *table);
    }
    memset(table, 0, tsize * sizeof *table);
    for (size_t t = 0; t + 2 < n; t += 3) {
        const Corner *a = &corners[t], *b = &corners[t + 1], *c = &corners[t + 2];
        float e1[3] = {b->w[0] - a->w[0], b->w[1] - a->w[1], b->w[2] - a->w[2]};
        float e2[3] = {c->w[0] - a->w[0], c->w[1] - a->w[1], c->w[2] - a->w[2]};
        float f[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float area = (b->sx - a->sx) * (c->sy - a->sy) - (b->sy - a->sy) * (c->sx - a->sx);
        if (area > 0)                   /* wound the other way on the screen: facing away as wound */
            f[0] = -f[0], f[1] = -f[1], f[2] = -f[2];
        for (int k = 0; k < 3; k++)
            memcpy(face[t + (size_t)k], f, sizeof f);
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t h[3];
        memcpy(h, corners[i].w, sizeof h);
        uint32_t x = (h[0] * 0x9E3779B1u ^ h[1] * 0x85EBCA77u ^ h[2] * 0xC2B2AE3Du);
        x ^= x >> 15;
        size_t j = x & (tsize - 1);
        for (;;) {
            uint32_t e = table[j];
            if (!e) {
                table[j] = (uint32_t)i + 1;
                owner[i] = (uint32_t)i;
                memcpy(sum[i], face[i], sizeof sum[i]);
                break;
            }
            if (!memcmp(corners[e - 1].w, corners[i].w, sizeof h)) {
                for (int k = 0; k < 3; k++)
                    sum[e - 1][k] += face[i][k];
                owner[i] = e - 1;
                break;
            }
            j = (j + 1) & (tsize - 1);
        }
    }
    GpuVertex *g = (GpuVertex *)((uint8_t *)vertices.map + batch_start);
    for (size_t i = 0; i < n; i++) {
        const float *s = sum[owner[i]], *f = face[i];
        float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]), fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (!(fl > 0))
            continue;
        const float *use = sl > 0 && (s[0] * f[0] + s[1] * f[1] + s[2] * f[2]) > 0.5f * sl * fl ? s : f;
        float k = 32767.0f / (use == s ? sl : fl);
        int16_t nn[4] = {(int16_t)(use[0] * k), (int16_t)(use[1] * k), (int16_t)(use[2] * k), 0};
        memcpy(g[i].n, nn, sizeof nn);
    }
}

static void flush(void) {
    VkDeviceSize n = (vertex_used - batch_start) / sizeof(GpuVertex);
    if (!n || !rendering) {
        extra_batch = extra_used;
        return;
    }
    if (shaded(&cur))
        make_normals((size_t)n);
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
    if (!last_valid || s->cull != last.cull)
        vkCmdSetCullMode(cmd, s->cull);
    if (!last_valid || s->front != last.front)
        vkCmdSetFrontFace(cmd, s->front);
    PushConstants pc = {s->tex_w ? s->tex_w : 1, s->tex_h ? s->tex_h : 1, s->tex_slot,
        shadow_on ? s->flags : s->flags & ~768u, s->alpha_test, s->env_color, s->fog_color, s->lights, {0},
        point_lights_on > 0 && (s->flags & 256u) && !(s->blend && s->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE)
            ? (uint32_t)vk.frame * LIGHT_SETS + LIGHT_SETS - 2 : ~0u, {0}};
    memcpy(pc.light, light_vp, sizeof pc.light);
    if (shaded(s))
        pc.flags |= 4096u;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cmd, (uint32_t)n, 1, (uint32_t)(batch_start / sizeof(GpuVertex)), 0);
    /* what is solid in the world casts shadows: drawn again into the shadow map - blended over what is there by its
     * alpha too (much of the scenery is), where it is more than half opaque */
    int alpha_blend = s->blend && s->eq.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
                      s->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (shadow_on && (s->flags & 256u) && s->depth_write && (!s->blend || alpha_blend)) {
        if (alpha_blend)
            pc.flags |= 2048u;
        shadow_begin();
        vkCmdPushConstants(shadow_cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc,
                           &pc);
        vkCmdDraw(shadow_cmd, (uint32_t)n, 1, (uint32_t)(batch_start / sizeof(GpuVertex)), 0);
        if (extra_used > extra_batch) {
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(shadow_cmd, 0, 1, &extra.buffer, &zero);
            vkCmdDraw(shadow_cmd, (uint32_t)((extra_used - extra_batch) / sizeof(GpuVertex)), 1,
                      (uint32_t)(extra_batch / sizeof(GpuVertex)), 0);
            vkCmdBindVertexBuffers(shadow_cmd, 0, 1, &vertices.buffer, &zero);
        }

    } else if (shadow_on && (s->flags & 256u))
        shadow_begin();                 /* (it is sampled: made this frame, if empty) */
    if (point_shadows && shadow_on && (s->flags & 256u)) {
        point_begin();
        if (s->depth_write && (!s->blend || alpha_blend)) {
            if (alpha_blend)
                pc.flags |= 2048u;
            vkCmdPushConstants(point_cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof pc, &pc);
            uint32_t instances = 6u * (uint32_t)point_shadows;
            vkCmdDraw(point_cmd, (uint32_t)n, instances, (uint32_t)(batch_start / sizeof(GpuVertex)), 0);
            if (extra_used > extra_batch) {
                VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(point_cmd, 0, 1, &extra.buffer, &zero);
                vkCmdDraw(point_cmd, (uint32_t)((extra_used - extra_batch) / sizeof(GpuVertex)), instances,
                          (uint32_t)(extra_batch / sizeof(GpuVertex)), 0);
                vkCmdBindVertexBuffers(point_cmd, 0, 1, &vertices.buffer, &zero);
            }
        }
    }
    last = *s;
    last_valid = 1;
    batch_start = vertex_used;
    extra_batch = extra_used;
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
    g->wx = v->wx;
    g->wy = v->wy;
    g->wz = v->wz;
    uint32_t sr = v->sr > 255 ? 255 : v->sr > 0 ? (uint32_t)v->sr : 0, sg = v->sg > 255 ? 255 : v->sg > 0 ? (uint32_t)v->sg : 0;
    uint32_t sb = v->sb > 255 ? 255 : v->sb > 0 ? (uint32_t)v->sb : 0;
    g->sun = sr | sg << 8 | sb << 16;
    float nl = sqrtf(v->nx * v->nx + v->ny * v->ny + v->nz * v->nz);
    float k = nl > 0 ? 32767.0f / nl : 0;
    g->n[0] = (int16_t)(v->nx * k);
    g->n[1] = (int16_t)(v->ny * k);
    g->n[2] = (int16_t)(v->nz * k);
    g->n[3] = 0;
    if (shaded(&cur)) {
        size_t i = (size_t)((vertex_used - batch_start) / sizeof(GpuVertex));
        if (i >= corners_cap) {
            corners_cap = corners_cap ? corners_cap * 2 : 65536;
            corners = realloc(corners, corners_cap * sizeof *corners);
        }
        corners[i] = (Corner){{v->wx, v->wy, v->wz}, x, y};
    }
    if (emitting) {
        double wgt = (double)(r + gg + b) * (double)a / (3.0 * 255.0 * 255.0);
        emit_sum[0] += wgt;
        emit_sum[1] += wgt * v->wx;
        emit_sum[2] += wgt * v->wy;
        emit_sum[3] += wgt * v->wz;
        emit_sum[4] += wgt * r / 255.0;
        emit_sum[5] += wgt * gg / 255.0;
        emit_sum[6] += wgt * b / 255.0;
    }
    vertex_used += sizeof(GpuVertex);
}

/* gpu.c: a triangle of the current primitive clipping cut (some of it is behind the camera): whole, for the shadows */
void render_shadow_triangle(const RVertex *a, const RVertex *b, const RVertex *c) {
    if (!shadow_on || !(cur.flags & 256u) || extra_used + 3 * sizeof(GpuVertex) > SHADOW_EXTRA_BYTES)
        return;
    const RVertex *v[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        GpuVertex *g = (GpuVertex *)((uint8_t *)extra.map + extra_used);
        memset(g, 0, sizeof *g);
        g->u = v[i]->u;
        g->v = v[i]->v;
        uint32_t al = v[i]->a < 0 ? 0 : v[i]->a > 255 ? 255 : (uint32_t)v[i]->a;
        g->color = al << 24;
        g->wx = v[i]->wx;
        g->wy = v[i]->wy;
        g->wz = v[i]->wz;
        extra_used += sizeof(GpuVertex);
    }
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
    ao_target = ao_applied = NULL;
    ao_used = 0;
    extra_used = extra_batch = 0;
    if (emitting)
        flush_emitter();
    emitting = 0;
    make_point_lights();
    if (point_lights_on > 0)                /* for the next frame (its half: vk.frame is the next's now) */
        memcpy((LightSet *)light_buf.map + (uint32_t)vk.frame * LIGHT_SETS + LIGHT_SETS - 2, point_lights,
               sizeof point_lights);
    point_shadows = point_lights_on > 0 ? point_shadows_next : 0;
    light_sets_used = 0;
    prim_lights = ~0u;
    vertex_used = batch_start = 0;
    staging_used = 0;
    last_valid = 0;
    frame_no++;
}

/* The end of a frame not shown (fast-forwarding): its work submitted, the frame's buffers and checks started again */
void render_finish_frame(void) {
    if (ao_target)
        ao_apply(ao_target);
    end_rendering();
    if (vk.recording)
        vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    end_frame();
}

void render_flip(uint32_t addr) {
    if (ao_target)
        ao_apply(ao_target);
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
    vk.rendered = vk.rendered_s[index % 8];
    VkCommandBuffer cmd = vk_cmd();
    vk_transition(&t->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii = {present_sampler, t->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, present_sets[vk.frame], 0, 0, 1,
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &present_sets[vk.frame], 0, NULL);
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

/* The displayed part of a framebuffer at the internal resolution (MHFU_DUMP_FULL): malloc'd, *w by *h */
uint32_t *render_readback_full(uint32_t addr, int *w_out, int *h_out) {
    if (ao_target)
        ao_apply(ao_target);
    end_rendering();
    Target *t = target(addr, 0);
    if (!t)
        return NULL;
    vk_transition(&t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uint32_t w = 480u * (uint32_t)scale, h = 272u * (uint32_t)scale;
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(vk_cmd(), t->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_finish();
    uint32_t *p = malloc((size_t)w * h * 4);
    const uint32_t *src = readback.map;
    for (size_t i = 0; i < (size_t)w * h; i++)
        p[i] = src[i] | 0xFF000000u;
    *w_out = (int)w;
    *h_out = (int)h;
    return p;
}

/* The displayed part of a framebuffer, for a frame dump: read back, and scaled down to 480 x 272 */
void render_readback(uint32_t addr, uint32_t *pixels) {
    if (ao_target)
        ao_apply(ao_target);
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
    vk_finish();
    const uint32_t *src = readback.map;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            pixels[y * 480 + x] = src[(uint32_t)(y * scale) * w + (uint32_t)(x * scale)] | 0xFF000000u;
}
