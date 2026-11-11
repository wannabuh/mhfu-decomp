/* The Vulkan renderer: see render.h; its parts: render_internal.h.
 *
 * Render targets: one per framebuffer address in VRAM, 512 x 512 PSP pixels at MHFU_SCALE times the resolution
 * (default 2), colour (RGBA8) and depth. Primitives are batched into draws while the GE's state stays the same; a draw's
 * state is set dynamically (one pipeline): blending, depth test and write, colour write mask, scissor; the texture is an
 * index into a bindless array (render_texture.c), with the texture function and alpha test in push constants. Frames
 * are submitted and waited for at each flip.
 *
 * As a batch is flushed, the effects take their part: the scenery's normals and the wind (render_shading.c), the feet
 * (render_scene.c), the shadow maps (render_shadow.c), the glow and motion (render_post.c); as the GE's state changes,
 * the world's occlusion (render_ao.c, then the grass) and post-processing when the world is drawn.
 *
 * Rounder characters: what is lit by the pixel (it has normals: the hunter, monsters, people) is drawn tessellated -
 * cut finer the longer its edges on the screen, curved by its normals (ge.tesc, ge.tese) - with the world's camera
 * (clip space from the world) in a light set of its own (MHFU_TESSELLATION=0: none).
 *
 * The game's round shadows under the hunter's and monsters' feet: left out while the sun's shadows are drawn
 * (MHFU_BLOB_SHADOWS=1: kept) - flat quads, blended over the ground without writing its depth, in a 128 x 128 CLUT8
 * texture, their colour a dark grey (25).
 *
 * Not yet: the framebuffers' contents in VRAM for the CPU (nothing reads them back but frame dumps), block transfers
 * into framebuffers, stencil, logic ops, the blend factors the hardware lacks (doubled alphas). */
#include "settings.h"
#include "render_internal.h"
#include "render_queue.h"

#include "grass.h"
#include "hle.h"
#include "post.h"
#include "glyphs.h"
#include "perf.h"
#include "pacing.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "ge.vert.inc"
#include "ge.frag.inc"
#include "ge_tl.frag.inc"
#include "ge.tesc.inc"
#include "ge.tese.inc"
#include "ge_tl.vert.inc"
#include "ge_tl.tesc.inc"
#include "present.vert.inc"
#include "present.frag.inc"

int render_on;
float render_scale = 2, render_wide = 1;
/* How strong the stage's sun is (gpu.c, from the light the game gives its models): 1 a clear day's - the sun's shadows
 * and the shading of what is turned from it are that much fainter under a dull sky, none in a cave (#52) */
float render_sun_strength = 1;
static Target targets[MAX_TARGETS];
VkBuf vertices, staging;
static VkBuf frame_vertices[2], frame_staging[2];
/* images replaced while the GPU may still draw with them: freed when their frame comes round again */
static VkImg retired[2][64];
static int nretired[2];
VkDeviceSize staging_used, batch_start;
static VkDeviceSize vertex_used;
static VkPipeline ge_pipeline, present_pipeline;
VkPipelineLayout ge_layout;
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout set_layout, present_set_layout;
static VkDescriptorPool pool;
VkDescriptorSet ge_set;
static VkDescriptorSet present_sets[2];
VkSampler samplers[8];
static VkSampler present_sampler;
unsigned long frame_no = 1;
VkShaderStageFlags pc_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
static int tess_on = -1;
static VkPipeline tess_pipeline, bound_pipeline;

/* The GE's vertex stage on the GPU (MHFU_GPU_TL, ge_tl.vert): the draws (TlDraw), in halves (a frame in flight each);
 * the vertices as the game gave them and the triangles' indices - a half each for what is sent as it is drawn, then
 * TL_SLOTS slots for what recorded frames keep (gpu.c: a frame's, sent once as it is recorded, drawn from while it is
 * the latest and the frames after are in flight). A batch of them is drawn in segments, a draw each (its index the
 * instance) */
#define TL_RECORDS 8192
#define TL_VERTICES (128u << 10)
#define TL_INDICES (1u << 20)
#define TL_SLOTS 4
#define TL_SLOT_VERTICES (128u << 10)
#define TL_SLOT_INDICES (512u << 10)
#define TL_SEGMENTS 4096
static VkPipeline tl_pipeline, tl_tess_pipeline;
static VkBuf tl_records;
VkBuf tl_vertices, tl_indices;
/* What the scenery's effects take of those vertices (BIND_TL_EXTRA, tl_scenery): the slots' made once a recorded frame,
 * as each part is first drawn - from the slots' vertices and indices kept here (the buffers are slow to read), each
 * triangle's made noted */
static VkBuf tl_extra;
static TlVertex *tl_slot_v[TL_SLOTS];
static uint32_t *tl_slot_i[TL_SLOTS];
static uint8_t *tl_slot_made[TL_SLOTS];
/* The batch's part in them, made as it is flushed (tl_scenery): its draws' triangles - the vertices, kept (a slot's,
 * or this frame's sent as drawn), their corners' indices among them, the vertex buffer's base - each with its draw and,
 * a slot's, where its triangles' made is noted */
typedef struct {
    const TlVertex *v;
    const uint32_t *idx;
    uint32_t ntri, base;
    const TlDraw *d;
    uint8_t *made;
} TlPart;
static TlPart *tl_parts;
static int tl_nparts, tl_parts_cap;
static TlVertex *tl_sent_v;
static uint32_t *tl_sent_i, tl_nsent_i;
static void tl_scenery(int shade, int wind);
static uint32_t tl_nrec, tl_nvert, tl_nidx;
static unsigned long tl_submits;                /* the submissions made (vk_submit: frame_reused) */
static unsigned long tl_slot_use[TL_SLOTS];     /* the submission a slot was last drawn from in (0: never) */
static TlDraw tl_last;                  /* the last draw given; its record this frame (~0u: none yet) */
static TlDraw *tl_drawn;                /* (this frame's records, kept: tl_scenery) */
static uint32_t tl_rec = ~0u;
static TlSegment tl_seg[TL_SEGMENTS];
static int tl_nseg;
static size_t tl_nfeet;                 /* (the batch's lit vertices given: render_scene.c's feet) */
static VkShaderModule tess_ctrl, tess_eval, tess_ctrl_tl;
float prim_view[16], prim_world_view[12];
static int prim_world;
static int blob_shadows, blob_prim;

static Target *rendering;            /* the target being rendered into, or NULL */
static DrawState cur;                /* the current primitive's state */
static DrawState last;               /* the last draw's, as set in the command buffer */
static int last_valid;
static VkBuf readback;

static void frame_reused(int frame);

/* Whether a draw may sway in the wind: what has holes in its texture in the world, lit beforehand (the scenery) -
 * which of its triangles are a plant's, sway_batch says */
static int swaying(const DrawState *d) {
    return sway > 0 && (d->flags & DRAW_KIND) == DRAW_WORLD && d->depth_write && d->tex_slot < MAX_TEXTURES &&
           tex_cutout[d->tex_slot];
}

/* Whether a draw is shaded by the sun: the solid scenery (not what has holes in its texture - leaves, grass, whose
 * cards' normals mean nothing - nor what writes no depth or adds to the picture) */
static int shaded(const DrawState *d) {
    return (d->flags & DRAW_KIND) == DRAW_WORLD && d->depth_write && sun_shading && shadow_on &&
           (d->tex_slot >= MAX_TEXTURES || !tex_cutout[d->tex_slot]) &&
           (!d->blend || (d->eq.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
                          d->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA));
}

/* Whether a draw's feet are found (render_scene.c): what the GE lights in the world (the hunter, monsters, people) -
 * for the wind's and the grass's bending away from them */
static int finds_feet(const DrawState *d) {
    return (sway > 0 || grass_on) && (d->flags & DRAW_KIND) > DRAW_WORLD;
}

/* -- setup -------------------------------------------------------------------------------------------------------- */

/* A pipeline: kind 0 a full-screen pass (present.vert), 1 the GE's, 2 the sun's shadow map's, 3 the point lights', 4 a
 * full-screen pass multiplied into what is there, 5 one added to it, 6 the GE's tessellated, 7 the grass's blades;
 * 8, 9, 10, 11 as 1, 2, 3, 6 with the GE's vertex stage on the GPU (ge_tl.vert: the vertices as the game gave them) */
VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout pl, VkFormat color, VkFormat depth,
                         int ge) {
    /* MHFU_SHADOW_CLAMP=1: the sun's shadow map clamps too, so that what is beyond its range towards the sun still casts
     * - a jungle's rings of trees, further up than the range, are cut off where the range ends, and their shade draws
     * in as the hunter walks (#28). Off by default: in open country the far scenery then casts as well, in great
     * hard-edged shapes over the hills (the jungle's area 2). What is beyond the range the other way lands on the
     * cleared depth, shading nothing */
    static int shadow_clamp = -1;
    if (shadow_clamp < 0)
        shadow_clamp = getenv("MHFU_SHADOW_CLAMP") && atoi(getenv("MHFU_SHADOW_CLAMP")) != 0;
    int tl = ge >= 8, clamp_depth = ge == 8 || ((ge == 2 || ge == 9) && vk.depth_clamp && shadow_clamp);
    ge = ge == 8 ? 1 : ge == 9 ? 2 : ge == 10 ? 3 : ge == 11 ? 6 : ge;
    VkPipelineShaderStageCreateInfo stages[4] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
         tl ? tess_ctrl_tl : tess_ctrl, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
         tess_eval, "main", NULL},
    };
    VkVertexInputBindingDescription bind = {0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[8] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24},
        {3, 0, VK_FORMAT_R32_SFLOAT, 28},
        {4, 0, VK_FORMAT_R32G32B32_SFLOAT, 32},
        {5, 0, VK_FORMAT_R8G8B8A8_UNORM, 44},
        {6, 0, VK_FORMAT_R16G16B16A16_SNORM, 48},
        {7, 0, VK_FORMAT_R16G16B16A16_SFLOAT, 56},
    };
    int verts = (ge >= 1 && ge <= 3) || ge == 6 || ge == 7;   /* (6: the GE's, tessellated; 7: grass blades) */
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
                                               verts ? 1u : 0u, &bind, verts ? 8u : 0u, attrs};
    VkVertexInputBindingDescription blade_bind = {0, sizeof(GrassBlade), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription blade_attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_UINT, 12},
    };
    if (ge == 7) {                      /* a blade an instance (grass.vert) */
        vi.pVertexBindingDescriptions = &blade_bind;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions = blade_attrs;
    }
    /* the vertices as the game gave them (TlVertex), at binding 1 (the GE's at 0 stay bound) */
    VkVertexInputBindingDescription tl_bind = {1, sizeof(TlVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription tl_attrs[8] = {
        {0, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TlVertex, pos)},
        {1, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TlVertex, normal)},
        {2, 1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(TlVertex, color)},
        {3, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(TlVertex, uv)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TlVertex, weights)},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TlVertex, weights) + 16},
        {6, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TlVertex, shape)},
        {7, 1, VK_FORMAT_R32_UINT, offsetof(TlVertex, edges)},
    };
    if (tl) {
        vi.pVertexBindingDescriptions = &tl_bind;
        vi.vertexAttributeDescriptionCount = 8;
        vi.pVertexAttributeDescriptions = tl_attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        ge == 6 ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : ge == 7 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                                                    : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, NULL, 0, 3};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1,
                                            NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = clamp_depth ? VK_TRUE : VK_FALSE;    /* (the near plane is gpu.c's: a clip distance) */
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
    if (ge == 5) {                      /* added to what is there */
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        att.srcAlphaBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
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
    if (ge == 2) {                      /* the shadow map's: depth only, its faces both ways (no bias: ge_frag.glsl's, what
                                         * receives the shadow, is enough, and one here moves a wall off its ground: #25) */
        ri.colorAttachmentCount = 0;
        cb.attachmentCount = 0;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn_present;
    }
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, ge == 6 ? 4u : 2u,
                                        stages, &vi, &ia, ge == 6 ? &ts : NULL, &vp, &rs, &ms, verts ? &ds : NULL, &cb,
                                        &dy, pl, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p;
    if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p) != VK_SUCCESS) {
        fprintf(stderr, "Vulkan: pipeline creation failed\n");
        exit(1);
    }
    return p;
}

/* The internal resolution follows the window's (MHFU_SCALE, MHFU_RESOLUTION: fixed); the window was resized */
static int follow_window, window_resized;

void render_window_resized(void) {
    render_queue_sync();                /* (called from outside: the renderer's own again) */
    window_resized = 1;
}

/* The scale that fits a window (or a resolution) of w x h exactly, 1 to 12: letterboxed to the PSP's shape - but one
 * a little wider than it (under 2%: 16:9 is 0.74% wider than 480:272) is filled, the picture drawn that much wider
 * (*wide; MHFU_FILL=0: letterboxed still, 2541 of 2560 across) */
static float fit_scale(uint32_t w, uint32_t h, float *wide) {
    float s = fminf((float)w / 480.0f, (float)h / 272.0f);
    s = s < 1 ? 1 : s > 12 ? 12 : s;
    float over = (float)w / (480.0f * s);
    const char *fill = getenv("MHFU_FILL");
    *wide = over > 1 && over <= 1.02f && (float)h == roundf(272.0f * s) && !(fill && !atoi(fill)) ? over : 1;
    return s;
}

/* The internal resolution as set (MHFU_RESOLUTION, else MHFU_SCALE; follow_window: neither, the window's) */
static int have_window, resolution_changed;
static float scale_setting(float *wide) {
    const char *s = getenv("MHFU_SCALE"), *res = getenv("MHFU_RESOLUTION");
    int rw, rh;
    follow_window = 0;
    *wide = render_wide;
    if (res && sscanf(res, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
        if (fminf((float)rw / 480.0f, (float)rh / 272.0f) >= 1 && fminf((float)rw / 480.0f, (float)rh / 272.0f) <= 12)
            return fit_scale((uint32_t)rw, (uint32_t)rh, wide);
    } else if (s && atof(s) >= 1 && atof(s) <= 12) {
        *wide = 1;
        return (float)atof(s);
    } else
        follow_window = have_window;
    return render_scale;
}

/* settings.c: it was changed - made so when the frame is next shown (render_flip) */
void render_resolution_changed(void) {
    resolution_changed = 1;
}

int render_init(void *sdl_window) {
    /* the internal resolution: by default the window's, its pixels as they are (the picture drawn 1:1, re-made as
     * the window is resized); or MHFU_RESOLUTION=WxH (its lines, or as wide as it is if that is less: 2560x1440 is
     * 5.29 times the PSP's), or MHFU_SCALE times the PSP's (fractions too) - 1 to 12 times, above the window's
     * filtered down to it */
    have_window = sdl_window != NULL;
    render_scale = scale_setting(&render_wide);
    if (!vk_init(sdl_window))
        return 0;
    if (follow_window && vk.surface)
        render_scale = fit_scale(vk.swap_extent.width, vk.swap_extent.height, &render_wide);

    tess_on = vk.tessellation && (!getenv("MHFU_TESSELLATION") || atoi(getenv("MHFU_TESSELLATION")) != 0);
    if (tess_on)
        pc_stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    /* the GE's descriptor set (shared.h's bindings): the textures bindless, an array filled as they are made */
    VkDescriptorSetLayoutBinding b[10] = {
        {BIND_TEXTURES, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {BIND_SHADOW_MAP, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {BIND_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, pc_stages, NULL},
        {BIND_POINT_SHADOWS, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {BIND_TRAIL, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {BIND_AO, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {BIND_SCENE_COLOR, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {BIND_SCENE_DEPTH, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {BIND_TL, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {BIND_TL_EXTRA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
    };
    VkDescriptorBindingFlags bf[10];
    for (int i = 0; i < 10; i++)
        bf[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bf[BIND_TEXTURES] |= VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 10, bf};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi,
                                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 10, b};
    vkCreateDescriptorSetLayout(vk.device, &dl, NULL, &set_layout);
    VkDescriptorSetLayoutBinding pb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       NULL};
    VkDescriptorSetLayoutCreateInfo pdl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &pb};
    vkCreateDescriptorSetLayout(vk.device, &pdl, NULL, &present_set_layout);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES + 8},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                      VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 3, 2, ps};
    vkCreateDescriptorPool(vk.device, &dpi, NULL, &pool);
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &set_layout};
    vkAllocateDescriptorSets(vk.device, &dai, &ge_set);
    dai.pSetLayouts = &present_set_layout;
    vkAllocateDescriptorSets(vk.device, &dai, &present_sets[0]);
    vkAllocateDescriptorSets(vk.device, &dai, &present_sets[1]);

    VkPushConstantRange pr = {pc_stages, 0, sizeof(DrawPush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout, 1, &pr};
    vkCreatePipelineLayout(vk.device, &pli, NULL, &ge_layout);
    VkPushConstantRange ppr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 6 * sizeof(float)};
    VkPipelineLayoutCreateInfo ppli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &present_set_layout,
                                       1, &ppr};
    vkCreatePipelineLayout(vk.device, &ppli, NULL, &present_layout);

    VkShaderModule vs = vk_shader(ge_vert, sizeof ge_vert), fs = vk_shader(ge_frag, sizeof ge_frag);
    ge_pipeline = make_pipeline(vs, fs, ge_layout, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 1);
    if (tess_on) {
        tess_ctrl = vk_shader(ge_tesc, sizeof ge_tesc);
        tess_eval = vk_shader(ge_tese, sizeof ge_tese);
        tess_pipeline = make_pipeline(vs, fs, ge_layout, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 6);
    }
    if (vk.surface) {
        VkShaderModule pvs = vk_shader(present_vert, sizeof present_vert);
        VkShaderModule pfs = vk_shader(present_frag, sizeof present_frag);
        present_pipeline = make_pipeline(pvs, pfs, present_layout, vk.swap_format, VK_FORMAT_UNDEFINED, 0);
    }
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
    readback = vk_buffer((VkDeviceSize)scaled_x(RT_SIZE) * scaled(RT_SIZE) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    VkShaderModule tvs = VK_NULL_HANDLE;
    if (vk.clip_distance) {             /* the GPU's vertex stage (MHFU_GPU_TL) */
        tvs = vk_shader(ge_tl_vert, sizeof ge_tl_vert);
        /* (its pixels read their triangles' corners if they can: the scenery's normals chosen as the CPU's are) */
        VkShaderModule tfs = vk.barycentric ? vk_shader(ge_tl_frag, sizeof ge_tl_frag) : fs;
        tl_pipeline = make_pipeline(tvs, tfs, ge_layout, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 8);
        if (tess_pipeline) {
            tess_ctrl_tl = vk_shader(ge_tl_tesc, sizeof ge_tl_tesc);
            tl_tess_pipeline = make_pipeline(tvs, fs, ge_layout, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 11);
        }
        tl_records = vk_buffer(2 * TL_RECORDS * sizeof(TlDraw), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
        tl_vertices = vk_buffer((2 * TL_VERTICES + TL_SLOTS * TL_SLOT_VERTICES) * sizeof(TlVertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
        tl_indices = vk_buffer((2 * TL_INDICES + TL_SLOTS * TL_SLOT_INDICES) * sizeof(uint32_t),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 1);
        tl_extra = vk_buffer((2 * TL_WIND + 2 * TL_VERTICES + TL_SLOTS * TL_SLOT_VERTICES) * 16,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
        tl_drawn = malloc(TL_RECORDS * sizeof *tl_drawn);
        tl_sent_v = malloc(TL_VERTICES * sizeof *tl_sent_v);
        tl_sent_i = malloc(TL_INDICES * sizeof *tl_sent_i);
        for (int k = 0; k < TL_SLOTS; k++) {
            tl_slot_v[k] = malloc(TL_SLOT_VERTICES * sizeof(TlVertex));
            tl_slot_i[k] = malloc(TL_SLOT_INDICES * sizeof(uint32_t));
            tl_slot_made[k] = calloc(TL_SLOT_INDICES / 3, 1);
        }
        VkDescriptorBufferInfo tbi[2] = {{tl_records.buffer, 0, VK_WHOLE_SIZE}, {tl_extra.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet tw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_TL, 0, 1,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &tbi[0], NULL},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_TL_EXTRA, 0, 1,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &tbi[1], NULL}};
        vkUpdateDescriptorSets(vk.device, 2, tw, 0, NULL);
    }

    /* the parts (render_internal.h) */
    texture_init();
    water_init();
    shadow_init();
    lights_init();
    ao_init();
    ao_water_setup(vs, tvs);
    post_setup(vs, fs, tess_pipeline != VK_NULL_HANDLE, tvs);
    grass_init();
    vk.frame_reused = frame_reused;
    render_on = 1;
    fprintf(stderr, "renderer: Vulkan, %ux%u internal resolution (%.3g times the PSP's)\n", scaled_x(480), scaled(272),
            (double)render_scale);
    return 1;
}

/* -- render targets ----------------------------------------------------------------------------------------------- */

void write_slot(uint32_t slot, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo ii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_TEXTURES, slot, 1,
                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &w, 0, NULL);
}

/* A render target's images, at the internal resolution, and its slot for sampling them */
static void target_images(Target *t) {
    uint32_t px = scaled(RT_SIZE), pw = scaled_x(RT_SIZE);
    t->color = vk_image(pw, px, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
    t->depth = vk_image(pw, px, VK_FORMAT_D32_SFLOAT,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT);
    write_slot(t->sample_slot, t->color.view, samplers[1 | 2 | 4]);
}

/* A new internal resolution (between frames): everything its size made again, the targets' pictures scaled across
 * (their depth cleared) */
static void render_rescale(float scale, float wide) {
    if (scale == render_scale && wide == render_wide)
        return;
    vkDeviceWaitIdle(vk.device);
    uint32_t old_px = scaled(RT_SIZE), old_pw = scaled_x(RT_SIZE);
    render_scale = scale;
    render_wide = wide;
    uint32_t px = scaled(RT_SIZE), pw = scaled_x(RT_SIZE);
    static VkImg old[2 * MAX_TARGETS];
    int nold = 0;
    VkCommandBuffer cmd = vk_cmd();
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        if (!t->used)
            continue;
        VkImg color = t->color;
        old[nold++] = t->color;
        old[nold++] = t->depth;
        target_images(t);
        if (!t->cleared)
            continue;
        vk_transition(&color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk_transition(&t->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)old_pw, (int32_t)old_px, 1}},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)pw, (int32_t)px, 1}}};
        vkCmdBlitImage(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->color.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        vk_transition(&t->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue far = {0.0f, 0};
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, t->depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &r);
    }
    vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_finish();
    for (int i = 0; i < nold; i++)
        vk_image_free(&old[i]);
    vk_buffer_free(&readback);
    readback = vk_buffer((VkDeviceSize)pw * px * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    ao_rescale();
    grass_bind_ao();
    post_rescale(render_scale, render_wide);
    fprintf(stderr, "renderer: %ux%u internal resolution (%.3g times the PSP's), the window's\n", scaled_x(480),
            scaled(272), (double)render_scale);
}

/* The window's size changed (or its swapchain is out of date): the swapchain again, and the internal resolution
 * with it if it follows the window */
static void window_changed(void) {
    window_resized = 0;
    vk_recreate_swapchain();
    if (follow_window) {
        float wide, to = fit_scale(vk.swap_extent.width, vk.swap_extent.height, &wide);
        render_rescale(to, wide);
    }
}

static void resolution_set(void) {
    resolution_changed = 0;
    float wide, to = scale_setting(&wide);
    if (follow_window && vk.surface)
        to = fit_scale(vk.swap_extent.width, vk.swap_extent.height, &wide);
    render_rescale(to, wide);
}

/* The render target at a VRAM address (made if create) */
Target *target_at(uint32_t addr, int create) {
    addr &= 0x041FFFF0u;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (targets[i].used && targets[i].addr == addr)
            return &targets[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (!targets[i].used) {
            Target *t = &targets[i];
            t->used = 1;
            t->addr = addr;
            t->sample_slot = texture_new_slot();
            target_images(t);
            return t;
        }
    fprintf(stderr, "renderer: too many render targets\n");
    exit(1);
}

void end_rendering(void) {
    if (!rendering)
        return;
    flush();
    vkCmdEndRendering(vk_cmd());
    rendering = NULL;
}

/* The rendering ended, what is batched not drawn: drawn later, once rendering again (render_post.c: a draw held back
 * until the post-processing is done) */
void end_rendering_kept(void) {
    if (!rendering)
        return;
    vkCmdEndRendering(vk_cmd());
    rendering = NULL;
}

void begin_rendering(Target *t) {
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
    uint32_t px = scaled(RT_SIZE), pw = scaled_x(RT_SIZE);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {pw, px}}, 1, 0, 1, &ca, &da, NULL};
    VkCommandBuffer cmd = vk_cmd();
    vkCmdBeginRendering(cmd, &ri);
    t->cleared = 1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_pipeline);
    bound_pipeline = ge_pipeline;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_layout, 0, 1, &ge_set, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &zero);
    VkViewport vp = {0, 0, (float)pw, (float)px, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    rendering = t;
    last_valid = 0;
}

Target *rendering_target(void) {
    return rendering;
}

/* After drawing something else in the GE's pass (the grass): its pipeline and vertices again, its state to set */
void ge_rebind(void) {
    VkCommandBuffer cmd = vk_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_pipeline);
    bound_pipeline = ge_pipeline;
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &zero);
    last_valid = 0;
}

/* After a pass of something else (post-processing): the GE's state to set again */
void ge_lost(void) {
    last_valid = 0;
}

/* -- textures ----------------------------------------------------------------------------------------------------- */

/* An image the GPU may still be drawing with: freed once the frame comes round again */
void retire(VkImg *i) {
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
    tl_submits++;
    vertices = frame_vertices[frame];
    staging = frame_staging[frame];
    shadow_frame_reused(frame);
    for (int i = 0; i < nretired[frame]; i++)
        vk_image_free(&retired[frame][i]);
    nretired[frame] = 0;
}

/* -- state -------------------------------------------------------------------------------------------------------- */

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

void unpack_rgb(uint32_t c, float *f) {
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
    render_queue_sync();                /* (called from outside: the renderer's own again) */
    return texture_slot(reg, w, h);
}

/* gpu.c: the next primitive's camera space to the screen and to the world (if it is in the world) */
void render_prim_view(const float *screen_from_view, const float *world_from_view) {
    memcpy(prim_view, screen_from_view, sizeof prim_view);
    memcpy(prim_world_view, world_from_view, sizeof prim_world_view);
}

void mul4(const float *a, const float *b, float *out) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++)
                v += a[4 * k + r] * b[4 * c + k];
            out[4 * c + r] = v;
        }
}

int invert4(const float *m, float *out) {
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

/* The world to the screen (PSP pixels, depth over 65535; by columns), with a camera (view: its space to the screen;
 * world_view: to the world, 3 x 4) */
int world_to_screen(const float *view, const float *world_view, float *m) {
    float wv[16] = {0}, vw[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 3; i++)
            wv[4 * c + i] = world_view[3 * c + i];
    wv[15] = 1;
    if (!invert4(wv, vw))
        return 0;
    mul4(view, vw, m);
    return 1;
}

/* And to the clip space, as put places vertices (x, y over half the target, less w) */
int world_to_clip(const float *view, const float *world_view, float *clip) {
    float m[16];
    if (!world_to_screen(view, world_view, m))
        return 0;
    for (int c = 0; c < 4; c++) {
        clip[4 * c + 0] = m[4 * c + 0] / (RT_SIZE / 2.0f) - m[4 * c + 3];
        clip[4 * c + 1] = m[4 * c + 1] / (RT_SIZE / 2.0f) - m[4 * c + 3];
        clip[4 * c + 2] = m[4 * c + 2];
        clip[4 * c + 3] = m[4 * c + 3];
    }
    return 1;
}

void render_prim_begin(const uint32_t *reg) {
    if (emitting)
        lights_flush_emitter(cur.tex_slot);            /* the last primitive's light (each its own, batched or not) */
    DrawState s;
    memset(&s, 0, sizeof s);
    uint32_t fb = 0x04000000u | ((reg[0x9C] & 0xFFFFF0) | ((reg[0x9D] & 0xFF0000) << 8)) % 0x200000u;
    s.target = target_at(fb, 1);
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
    s.flags = (reg[0xC9] & DRAW_TFUNC) | (((reg[0xC9] >> 8) & 1) ? DRAW_TEX_ALPHA : 0u) |
              (((reg[0xC9] >> 16) & 1) ? DRAW_DOUBLE : 0u) | (!clear && (reg[0x22] & 1) ? DRAW_ALPHA_TEST : 0u) |
              (clear ? DRAW_CLEAR : 0u) | (!clear && (reg[0x1F] & 1) && !((reg[0x12] >> 23) & 1) ? DRAW_FOG : 0u);
    s.fog_color = reg[0xCF];
    s.alpha_test = reg[0xDB];
    s.cull = VK_CULL_MODE_NONE;
    s.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    s.lights = ~0u;
    if (prim_world && !clear) {
        s.flags |= DRAW_WORLD | ((reg[0x17] & 1) ? (prim_light_set() != ~0u ? DRAW_LIT_PIXEL : DRAW_LIT_GE) : 0u);
        s.lights = prim_light_set();
        if (reg[0xFE] & 6)              /* (gpu.c: far scenery, an effect) */
            s.flags |= DRAW_NO_CAST;
        if (reg[0x1D] & 1) {            /* the culling gpu.c leaves to the GPU (the same faces as it would drop) */
            s.cull = VK_CULL_MODE_BACK_BIT;
            s.front = (reg[0x9B] & 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        }
    } else if (!clear && !((reg[0x12] >> 23) & 1) && (reg[0x1D] & 1)) {
        /* 3D not placed in the world: culled by gpu.c, its faces dropped - on the GPU too (its vertex stage there:
         * MHFU_GPU_TL) */
        s.cull = VK_CULL_MODE_BACK_BIT;
        s.front = (reg[0x9B] & 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
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
    s.scissor = (VkRect2D){{(int32_t)scaled_x(sx0), (int32_t)scaled(sy0)}, {scaled_x(sx1) - scaled_x(sx0), scaled(sy1) - scaled(sy0)}};
    /* the solid world drawn, then what is not: its occlusion (and the grass), then post-processing; its fog and depth
     * test noted (the grass's) */
    /* (not what is see-through all over though it writes depth - dust and the like, #30: blended by its alpha, unlit,
     * no colours of its vertices' own and the material's alpha under full. Its squares' depth would be taken for
     * ground and the occlusion laid over them) */
    int sheer = s.blend && (reg[0xDF] & 0xFFF) == 0x32 && !(reg[0x17] & 1) && !((reg[0x12] >> 2) & 7) && (reg[0x58] & 0xFF) < 0xFF;
    /* (nor what keeps what is behind it whatever its own alpha - adds to it, or blends with it by a fixed share: a
     * layer of light or mist, as the forest's over all of the picture, close to the camera, #72) */
    sheer |= s.blend && (s.eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE ||
                         s.eq.dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
                         s.eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR);
    int solid = (s.flags & DRAW_WORLD) && s.depth_write && !sheer;
    ao_state(s.target, solid, clear);
    if (solid) {
        scene.depth_op = s.depth_op;
        uint32_t fe = reg[0xCD] << 8, fs = reg[0xCE] << 8;
        memcpy(&scene.fog[0], &fe, 4);
        memcpy(&scene.fog[1], &fs, 4);
        scene.fog[2] = (s.flags & DRAW_FOG) ? 1.0f : 0.0f;
        scene.fog_color = s.fog_color;
    }
    if (!clear && water_prim(reg, &s))
        s.flags |= DRAW_WATER;
    post_state(s.target, s.flags, solid, s.depth_write, clear, !memcmp(&s, &cur, sizeof s));
    if (memcmp(&s, &cur, sizeof s)) {
        flush();
        cur = s;
    }
    static unsigned settings_seen;
    if (SETTING_STALE(settings_seen)) {
        lights_settings();
        shadow_settings();
        shading_settings();
        water_settings();
        grass_setting();
        blob_shadows = getenv("MHFU_BLOB_SHADOWS") && atoi(getenv("MHFU_BLOB_SHADOWS")) != 0;
    }
    emitting = point_lights_on && (s.flags & DRAW_WORLD) && s.blend && s.eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE &&
               s.eq.colorBlendOp == VK_BLEND_OP_ADD;
    blob_prim = !blob_shadows && (s.flags & DRAW_KIND) == DRAW_WORLD && s.blend && !s.depth_write && (reg[0xDF] & 0xFFF) == 0x32 &&
                (reg[0xC3] & 0xF) == 5 && (reg[0xB8] & 0xF0F) == 0x707;
}

/* gpu.c: the next primitive's vertices have their place in the world (shadows fall on it, and it casts them) */
void render_prim_world(int on) {
    prim_world = on;
}

static uint32_t tl_wind(void);

/* A batch of the GPU's vertex stage drawn: each segment a draw, its record the instance */
static void flush_tl(VkCommandBuffer cmd, const DrawState *s, DrawPush *pc) {
    if (bound_pipeline != tl_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl_pipeline);
        bound_pipeline = tl_pipeline;
    }
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 1, 1, &tl_vertices.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, tl_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    pc->camera = (s->flags & DRAW_WATER) ? water_light_set() : ~0u;
    if (shaded(s)) {
        pc->flags |= DRAW_SHADED | DRAW_FACES | (relief_on && s->tex_slot < MAX_TEXTURES ? DRAW_RELIEF : 0u);
    } else if (swaying(s) && !(s->flags & DRAW_WATER)) {
        pc->flags |= DRAW_SWAY;
        pc->camera = tl_wind();
    }
    if (s->flags & DRAW_WORLD) {
        float j[2] = {render_jitter[0] * 2.0f / (float)scaled_x(RT_SIZE), render_jitter[1] * 2.0f / (float)scaled(RT_SIZE)};
        memcpy(&pc->pad0, &j[0], 4);
        memcpy(&pc->pad1, &j[1], 4);
    }
    vkCmdPushConstants(cmd, ge_layout, pc_stages, 0, sizeof *pc, pc);
    for (int i = 0; i < tl_nseg; i++)
        vkCmdDrawIndexed(cmd, tl_seg[i].count, 1, tl_seg[i].first, 0, tl_seg[i].rec);
    /* lit by the pixel, opaque: also drawn tessellated, over itself drawn flat (as flush draws the CPU's batches) */
    uint32_t cam = tl_tess_pipeline && (s->flags & DRAW_LIT_PIXEL) && !s->blend ? camera_light_set() : ~0u;
    if (cam != ~0u) {
        pc->camera = cam;
        vkCmdPushConstants(cmd, ge_layout, pc_stages, 0, sizeof *pc, pc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl_tess_pipeline);
        bound_pipeline = tl_tess_pipeline;
        for (int i = 0; i < tl_nseg; i++)
            vkCmdDrawIndexed(cmd, tl_seg[i].count, 1, tl_seg[i].first, 0, tl_seg[i].rec);
    }
    if (tl_nfeet)
        scene_lit_batch(tl_nfeet);
    tl_nfeet = 0;
    post_keep_tl(rendering, s, pc, tl_seg, tl_nseg);
    ao_keep(rendering, s, pc, 0, (uint32_t)tl_nseg, tl_seg);
    shadow_draw_tl(s, *pc, tl_seg, tl_nseg);
    tl_nseg = 0;
}

/* Draw what has been batched with the current state */
void flush(void) {
    VkDeviceSize n = (vertex_used - batch_start) / sizeof(GpuVertex);
    if ((!n && !tl_nseg) || !rendering) {
        shadow_skip();
        tl_nseg = 0;
        tl_nfeet = 0;
        tl_nparts = 0;
        return;
    }
    tl_scenery(shaded(&cur), swaying(&cur));
    if (n && shaded(&cur))
        make_normals((size_t)n);
    if (n && swaying(&cur))
        sway_batch(&cur, (size_t)n);
    if (n && finds_feet(&cur))
        scene_lit_batch((size_t)n);
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
    DrawPush pc = {{s->tex_w ? s->tex_w : 1, s->tex_h ? s->tex_h : 1}, s->tex_slot,
        shadow_on ? s->flags : s->flags & ~(DRAW_WORLD | DRAW_LIT_GE), s->alpha_test, s->env_color, s->fog_color, s->lights, {0},
        point_lights_on > 0 && (s->flags & DRAW_WORLD) && !(s->blend && s->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE)
            ? point_light_set() : ~0u, ~0u, 0, 0};
    memcpy(pc.light, scene.sun_vp, sizeof pc.light);
    pc.light[15] = render_sun_strength;     /* (the matrix's last row is not used: its place for it) */
    if (tl_nseg) {                      /* the GPU's vertex stage: its segments (none of the effects' work yet) */
        flush_tl(cmd, s, &pc);
        last = *s;
        last_valid = 1;
        return;
    }
    /* lit by the pixel, opaque: also drawn tessellated (rounder), over itself drawn flat - which shows through any
     * crack the curving opens (and wins where it would curve inwards) */
    uint32_t cam = tess_pipeline && (s->flags & DRAW_LIT_PIXEL) && !s->blend && n % 3 == 0 && on_screen_size((size_t)n) >= 40
                       ? camera_light_set() : ~0u;
    if (cam != ~0u)
        shape_normals((size_t)n);
    else if (tess_pipeline && (s->flags & DRAW_LIT_PIXEL))
        memcpy((uint8_t *)vertices.map + batch_start, shading_kept(), (size_t)n * sizeof(GpuVertex));
    if (bound_pipeline != ge_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_pipeline);
        bound_pipeline = ge_pipeline;
    }
    pc.camera = cam;
    if (s->flags & DRAW_WATER)
        pc.camera = water_light_set();
    if (s->flags & DRAW_WORLD) {
        float j[2] = {render_jitter[0] * 2.0f / (float)scaled_x(RT_SIZE), render_jitter[1] * 2.0f / (float)scaled(RT_SIZE)};
        memcpy(&pc.pad0, &j[0], 4);
        memcpy(&pc.pad1, &j[1], 4);
    }
    if (shaded(s))
        pc.flags |= DRAW_SHADED | (relief_on && s->tex_slot < MAX_TEXTURES ? DRAW_RELIEF : 0u);
    vkCmdPushConstants(cmd, ge_layout, pc_stages, 0, sizeof pc, &pc);
    vkCmdDraw(cmd, (uint32_t)n, 1, (uint32_t)(batch_start / sizeof(GpuVertex)), 0);
    if (cam != ~0u) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tess_pipeline);
        bound_pipeline = tess_pipeline;
        vkCmdDraw(cmd, (uint32_t)n, 1, (uint32_t)(batch_start / sizeof(GpuVertex)), 0);
    }
    uint32_t first = (uint32_t)(batch_start / sizeof(GpuVertex));
    post_keep(rendering, s, &pc, first, (uint32_t)n, cam != ~0u);
    ao_keep(rendering, s, &pc, first, (uint32_t)n, NULL);
    shadow_draw(s, pc, first, (uint32_t)n);
    last = *s;
    last_valid = 1;
    batch_start = vertex_used;
}

/* -- primitives --------------------------------------------------------------------------------------------------- */

/* -- primitives --------------------------------------------------------------------------------------------------- */

/* a float as a half float (rounded towards zero; too large: the largest) */
static uint16_t half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint16_t sign = (uint16_t)((x >> 16) & 0x8000);
    int e = (int)((x >> 23) & 255) - 127 + 15;
    if (e <= 0)
        return sign;
    if (e >= 31)
        return sign | 0x7BFF;
    return sign | (uint16_t)(e << 10) | (uint16_t)((x >> 13) & 0x3FF);
}

static void put(const RVertex *v, float x, float y, float u, float vv, int sprite) {
    if (vertex_used + sizeof(GpuVertex) > VERTEX_BYTES)
        return;
    GpuVertex vert, *g = &vert;         /* (made here, written to the mapped buffer once: it is slow to read) */
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
    g->motion[0] = half(v->mx);
    g->motion[1] = half(v->my);
    g->motion[2] = half(v->mz);
    g->motion[3] = 0;
    /* what the effects take of it (render_shading.c, render_scene.c, render_lights.c) */
    size_t i = (size_t)((vertex_used - batch_start) / sizeof(GpuVertex));
    int tess = tess_pipeline && (cur.flags & DRAW_LIT_PIXEL);     /* (written at flush, with its shape normals) */
    const float world[3] = {v->wx, v->wy, v->wz};
    if (swaying(&cur) || tess)
        shading_keep(i, &vert);
    if (shaded(&cur))
        shading_corner(i, world, x, y);
    if (finds_feet(&cur))
        scene_lit_vertex(i, world);
    if (emitting)
        lights_emit(world, r, gg, b, a);
    if (!tess)
        memcpy((uint8_t *)vertices.map + vertex_used, &vert, sizeof vert);
    vertex_used += sizeof(GpuVertex);
}

/* gpu.c: a triangle of the current primitive clipping cut (some of it is behind the camera): whole, for the shadows */
void render_shadow_triangle(const RVertex *a, const RVertex *b, const RVertex *c) {
    if (shadow_on && (cur.flags & DRAW_WORLD))
        shadow_extra_triangle(a, b, c);
}

static int blob_vertex(const RVertex *v) {
    return v->r == 25.0f && v->g == 25.0f && v->b == 25.0f;
}

void render_triangle(const RVertex *a, const RVertex *b, const RVertex *c) {
    if (blob_prim && shadow_on && blob_vertex(a) && blob_vertex(b) && blob_vertex(c))
        return;
    begin_rendering(cur.target);
    if (tl_nseg)
        flush();                        /* (the GPU's vertex stage's batch first: the order drawn kept) */
    put(a, a->x, a->y, a->u, a->v, 0);
    put(b, b->x, b->y, b->u, b->v, 0);
    put(c, c->x, c->y, c->u, c->v, 0);
    if (grass_on && shaded(&cur) && ((cur.flags & DRAW_TFUNC) == 0 || (cur.flags & DRAW_TFUNC) == 3))
        grass_scenery(&cur, a, b, c);
}

/* A sprite: the rectangle between two corners, as two triangles, in the second corner's colour and depth */
void render_sprite(const RVertex *a, const RVertex *b) {
    begin_rendering(cur.target);
    if (tl_nseg)
        flush();
    RVertex v = *b;
    v.rw = 1.0f;
    put(&v, a->x, a->y, a->u, a->v, 1);
    put(&v, b->x, a->y, b->u, a->v, 1);
    put(&v, a->x, b->y, a->u, b->v, 1);
    put(&v, b->x, a->y, b->u, a->v, 1);
    put(&v, b->x, b->y, b->u, b->v, 1);
    put(&v, a->x, b->y, a->u, b->v, 1);
}

/* -- the GE's vertex stage on the GPU (MHFU_GPU_TL) -------------------------------------------------------------- */

int render_tl_ready(void) {
    return tl_pipeline != VK_NULL_HANDLE;
}

/* (gpu.c) whether the feet of its draws lit in the world are wanted (finds_feet) */
int render_tl_feet(void) {
    return sway > 0 || grass_on;
}

/* The draw's lowest vertices, for the batch's feet (as put gives scene_lit_vertex its vertices) */
static void tl_feet(const float *feet, int nfeet) {
    for (int k = 0; k < nfeet; k++)
        scene_lit_vertex(tl_nfeet++, feet + 3 * k);
}

/* (gpu.c) whether its draws lit by the pixel are tessellated: their shape normals wanted */
int render_tl_tessellates(void) {
    return tl_tess_pipeline != VK_NULL_HANDLE;
}

/* The draw given last's record this frame (made if need be), ~0u if there is no room */
static uint32_t tl_record(const TlDraw *d) {
    if (d) {                            /* (taken whatever is drawn: gpu.c sends a draw once) */
        memcpy(&tl_last, d, sizeof tl_last);
        if (tl_last.wounds)             /* (its wounds' picture: where the pixel stage finds it) */
            tl_last.wound_pad0 = texture_wound_slot();
        tl_rec = ~0u;
    }
    if (tl_rec == ~0u) {
        if (tl_nrec >= TL_RECORDS) {
            static int warned;
            if (!warned++)
                fprintf(stderr, "renderer: the GPU vertex stage's draws a frame (%d) run out\n", TL_RECORDS);
            return ~0u;
        }
        tl_rec = (uint32_t)vk.frame * TL_RECORDS + tl_nrec++;
        memcpy((TlDraw *)tl_records.map + tl_rec, &tl_last, sizeof tl_last);
        memcpy(&tl_drawn[tl_rec % TL_RECORDS], &tl_last, sizeof tl_last);
    }
    return tl_rec;
}

/* The batch takes n indices from first, drawn with record rec (joined to the segment before if they follow on) */
static void tl_segment(uint32_t first, uint32_t n, uint32_t rec) {
    if (tl_nseg && tl_seg[tl_nseg - 1].rec == rec && tl_seg[tl_nseg - 1].first + tl_seg[tl_nseg - 1].count == first) {
        tl_seg[tl_nseg - 1].count += n;
        return;
    }
    if (tl_nseg == TL_SEGMENTS)
        flush();
    tl_seg[tl_nseg].first = first;
    tl_seg[tl_nseg].count = n;
    tl_seg[tl_nseg++].rec = rec;
}

/* (a draw of the GPU's vertex stage begins) its target, what the CPU's vertex stage batched drawn first */
static void tl_begin(void) {
    begin_rendering(cur.target);
    if (vertex_used > batch_start)
        flush();                        /* (the order drawn kept) */
}

/* The frame's wind for the draws it moves (DRAW_SWAY; shared.h TL_WIND): its time, how far, the feet among the plants
 * (render_scene.c's) - the first vec4 of it in BIND_TL_EXTRA */
static uint32_t tl_wind(void) {
    static unsigned long made;
    uint32_t at = (uint32_t)vk.frame * TL_WIND;
    if (made != frame_no) {
        made = frame_no;
        float *p = (float *)tl_extra.map + 4 * at;
        int nfeet = scene.nfeet < (int)TL_WIND - 1 ? scene.nfeet : (int)TL_WIND - 1;
        memcpy(p, (float[4]){(float)fmod(scene_time(), 3600.0), 7.0f * sway, (float)nfeet, 0}, 16);
        for (int f = 0; f < nfeet; f++) {
            const Foot *ft = &scene.feet[f];
            memcpy(p + 4 * (1 + f), (float[4]){ft->x, ft->y, ft->z, ft->r}, 16);
        }
    }
    return at;
}

/* Where a vertex is in the world, as ge_tl.vert places it (without the wind) */
static void tl_world_at(const TlDraw *d, const TlVertex *v, float *w) {
    float p[3] = {0, 0, 0}, m[3];
    if (d->bones) {
        for (uint32_t b = 0; b < d->bones; b++)
            for (int r = 0; r < 3; r++) {
                const float *row = d->bone[3 * b + (uint32_t)r];
                p[r] += (row[0] * v->pos[0] + row[1] * v->pos[1] + row[2] * v->pos[2] + row[3]) * v->weights[b];
            }
    } else {
        memcpy(p, v->pos, sizeof p);
    }
    for (int r = 0; r < 3; r++)
        m[r] = d->world[r][0] * p[0] + d->world[r][1] * p[1] + d->world[r][2] * p[2] + d->world[r][3];
    for (int r = 0; r < 3; r++)
        w[r] = d->to_world[r][0] * m[0] + d->to_world[r][1] * m[1] + d->to_world[r][2] * m[2] + d->to_world[r][3];
}

/* A draw's triangles into the batch's part in the scenery's effects */

static void tl_part(const TlVertex *v, const uint32_t *idx, uint32_t ntri, uint32_t base, uint8_t *made) {
    if (tl_nparts == tl_parts_cap) {
        tl_parts_cap = tl_parts_cap ? tl_parts_cap * 2 : 256;
        tl_parts = realloc(tl_parts, sizeof *tl_parts * (size_t)tl_parts_cap);
    }
    tl_parts[tl_nparts++] = (TlPart){v, idx, ntri, base, &tl_drawn[tl_rec % TL_RECORDS], made};
}

/* Where the vertices are in the world, without the wind (ge_tl.vert): the corners of a part's triangles */
static void tl_corners(const TlPart *pt, float (*cw)[3]) {
    for (uint32_t c = 0; c < 3 * pt->ntri; c++)
        tl_world_at(pt->d, &pt->v[pt->idx[c]], cw[c]);
}

/* The batch's part in the scenery's effects made (its state's shade, wind), as the CPU's vertex stage's batches have
 * it (render_shading.c) - its corners welded whatever draw they came in. shade: the sun shading's normals (make_normals:
 * each triangle's turned towards the camera, summed where corners meet - the same place in the world; ge.frag takes a
 * triangle's own where that bends more than 60 degrees from it, DRAW_FACES). wind: the heights it moves them by
 * (sway_batch: above the lowest foot of the triangles meeting there; held where one is not a plant's). Once for a
 * slot's triangles (the scenery stands still: unless a part is new, the batch is as made before) */
static void tl_scenery(int shade, int wind) {
    static float (*cw)[3], (*sum)[3], *foot;
    static uint32_t *owner, *table;
    static uint8_t *held;
    static size_t cap, table_cap;
    int nparts = tl_nparts, fresh = 0;
    tl_nparts = 0;
    if (!shade && !wind)
        return;
    size_t n = 0;
    for (int k = 0; k < nparts; k++) {
        n += 3 * (size_t)tl_parts[k].ntri;
        fresh |= !tl_parts[k].made || !tl_parts[k].made[0];
    }
    if (!n || !fresh)
        return;
    if (n > cap) {
        cap = n * 2;
        cw = realloc(cw, cap * sizeof *cw);
        sum = realloc(sum, cap * sizeof *sum);
        foot = realloc(foot, cap * sizeof *foot);
        owner = realloc(owner, cap * sizeof *owner);
        held = realloc(held, cap);
    }
    for (int k = 0, c = 0; k < nparts; c += 3 * (int)tl_parts[k++].ntri)
        tl_corners(&tl_parts[k], cw + c);
    /* the places: each corner's first at its place */
    size_t tsize = 1024;
    while (tsize < n * 2)
        tsize *= 2;
    if (tsize > table_cap) {
        table_cap = tsize;
        table = realloc(table, table_cap * sizeof *table);
    }
    memset(table, 0, tsize * sizeof *table);
    for (size_t c = 0; c < n; c++) {
        uint32_t hb[3];
        memcpy(hb, cw[c], sizeof hb);
        uint32_t x = hb[0] * 0x9E3779B1u ^ hb[1] * 0x85EBCA77u ^ hb[2] * 0xC2B2AE3Du;
        x ^= x >> 15;
        size_t j = x & (tsize - 1);
        for (;;) {
            uint32_t e = table[j];
            if (!e) {
                table[j] = (uint32_t)c + 1;
                owner[c] = (uint32_t)c;
                break;
            }
            if (!memcmp(cw[e - 1], cw[c], sizeof hb)) {
                owner[c] = e - 1;
                break;
            }
            j = (j + 1) & (tsize - 1);
        }
        sum[c][0] = sum[c][1] = sum[c][2] = 0;
        foot[c] = 1e30f;
        held[c] = 0;
    }
    float (*out)[4] = (float (*)[4])tl_extra.map + 2 * TL_WIND;    /* (write-combined: written, never read) */
    int any = 0;
    const uint8_t *holes = cur.tex_slot < MAX_TEXTURES ? tex_holes[cur.tex_slot] : NULL;
    float tw = cur.tex_w > 0 ? cur.tex_w : 1, th = cur.tex_h > 0 ? cur.tex_h : 1;
    for (int k = 0, c0 = 0; k < nparts; c0 += 3 * (int)tl_parts[k++].ntri) {
        const TlPart *pt = &tl_parts[k];
        const TlDraw *d = pt->d;
        if (shade) {
            /* the camera's place in the world: the view undone (the origin of its space), then to_world */
            const float (*vw)[4] = (const float (*)[4])d->view;
            float r[3][3], e[3], t[3] = {vw[0][3], vw[1][3], vw[2][3]};
            for (int i = 0; i < 3; i++)
                for (int q = 0; q < 3; q++)
                    r[i][q] = vw[i][q];
            float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                        r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) + r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
            if (fabsf(det) < 1e-12f)
                det = 1e-12f;
            float inv[3][3] = {
                {(r[1][1] * r[2][2] - r[1][2] * r[2][1]) / det, (r[0][2] * r[2][1] - r[0][1] * r[2][2]) / det,
                 (r[0][1] * r[1][2] - r[0][2] * r[1][1]) / det},
                {(r[1][2] * r[2][0] - r[1][0] * r[2][2]) / det, (r[0][0] * r[2][2] - r[0][2] * r[2][0]) / det,
                 (r[0][2] * r[1][0] - r[0][0] * r[1][2]) / det},
                {(r[1][0] * r[2][1] - r[1][1] * r[2][0]) / det, (r[0][1] * r[2][0] - r[0][0] * r[2][1]) / det,
                 (r[0][0] * r[1][1] - r[0][1] * r[1][0]) / det}};
            float m[3];
            for (int i = 0; i < 3; i++)
                m[i] = -(inv[i][0] * t[0] + inv[i][1] * t[1] + inv[i][2] * t[2]);
            for (int i = 0; i < 3; i++)
                e[i] = d->to_world[i][0] * m[0] + d->to_world[i][1] * m[1] + d->to_world[i][2] * m[2] + d->to_world[i][3];
            static int by_camera = -1;
            if (by_camera < 0)
                by_camera = getenv("MHFU_NORMALS_BY_CAMERA") && atoi(getenv("MHFU_NORMALS_BY_CAMERA")) != 0;
            float det3 = 1;
            for (int pass = 0; pass < 2; pass++) {      /* (the model to the world: its matrix, then the camera undone) */
                const float (*w3)[4] = pass ? (const float (*)[4])d->to_world : (const float (*)[4])d->world;
                det3 *= w3[0][0] * (w3[1][1] * w3[2][2] - w3[1][2] * w3[2][1]) -
                        w3[0][1] * (w3[1][0] * w3[2][2] - w3[1][2] * w3[2][0]) +
                        w3[0][2] * (w3[1][0] * w3[2][1] - w3[1][1] * w3[2][0]);
            }
            int mirrored = det3 < 0;
            for (uint32_t tr = 0; tr < pt->ntri; tr++) {
                size_t c = (size_t)c0 + 3 * tr;
                const float *a = cw[c], *b = cw[c + 1], *cc = cw[c + 2];
                float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {cc[0] - a[0], cc[1] - a[1], cc[2] - a[2]};
                float f[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                /* Which way it faces: as it is wound (the scenery's triangles are wound alike, their fronts out -
                 * all but a handful of 225000 in the volcano's cave), a mirrored model's the other way. Not towards
                 * the camera, as before: a rock's far side, turned to face whoever first saw it, gave the corners
                 * round its edge normals that depended on where the camera was when its mesh came into view - its
                 * triangles lit or dark by the angle (#52). MHFU_NORMALS_BY_CAMERA=1: as before */
                int flip = by_camera ? f[0] * (e[0] - a[0]) + f[1] * (e[1] - a[1]) + f[2] * (e[2] - a[2]) < 0 : mirrored;
                if (flip)
                    f[0] = -f[0], f[1] = -f[1], f[2] = -f[2];
                for (int q = 0; q < 3; q++)
                    for (int i = 0; i < 3; i++)
                        sum[owner[c + (size_t)q]][i] += f[i];
            }
        } else {
            /* a plant's: the part of the texture it shows is mostly holes and a plant's colour (sway_batch) */
            int tex = (d->flags & TL_TEX) != 0;
            for (uint32_t tr = 0; tr < pt->ntri; tr++) {
                size_t c = (size_t)c0 + 3 * tr;
                float uv[3][2];
                for (int q = 0; q < 3; q++) {
                    const TlVertex *x = &pt->v[pt->idx[3 * tr + (uint32_t)q]];
                    uv[q][0] = tex ? x->uv[0] * d->tex[0] + d->tex[2] : 0;
                    uv[q][1] = tex ? x->uv[1] * d->tex[1] + d->tex[3] : 0;
                }
                float cu = (uv[0][0] + uv[1][0] + uv[2][0]) / 3, cv = (uv[0][1] + uv[1][1] + uv[2][1]) / 3;
                int share = 0;
                for (int q = -1; q < 3 && holes; q++) {
                    float u = q < 0 ? cu : 0.6f * uv[q][0] + 0.4f * cu, vv = q < 0 ? cv : 0.6f * uv[q][1] + 0.4f * cv;
                    u /= tw;
                    vv /= th;
                    u -= floorf(u);
                    vv -= floorf(vv);
                    int cx = (int)(u * HOLE_GRID), cy = (int)(vv * HOLE_GRID);
                    share += holes[(cy < HOLE_GRID ? cy : HOLE_GRID - 1) * HOLE_GRID + (cx < HOLE_GRID ? cx : HOLE_GRID - 1)];
                }
                int plant = share >= 4 * 70;
                any |= plant;
                float f = fminf(cw[c][1], fminf(cw[c + 1][1], cw[c + 2][1]));
                for (int q = 0; q < 3; q++) {
                    uint32_t o = owner[c + (size_t)q];
                    foot[o] = fminf(foot[o], f);
                    held[o] |= !plant;
                }
            }
        }
    }
    for (int k = 0, c0 = 0; k < nparts; c0 += 3 * (int)tl_parts[k++].ntri) {
        const TlPart *pt = &tl_parts[k];
        for (size_t i = 0; i < 3 * (size_t)pt->ntri; i++) {
            size_t c = (size_t)c0 + i;
            float x[4] = {0, 0, 0, 0};
            if (shade) {
                const float *sv = sum[owner[c]];
                float sl = sqrtf(sv[0] * sv[0] + sv[1] * sv[1] + sv[2] * sv[2]);
                if (sl > 0)
                    x[0] = sv[0] / sl, x[1] = sv[1] / sl, x[2] = sv[2] / sl;
            } else {
                float h = cw[c][1] - foot[owner[c]];
                x[0] = any && !held[owner[c]] && h > 0.5f ? h : 0;
            }
            memcpy(out[pt->base + pt->idx[i]], x, sizeof x);
        }
        if (pt->made)
            memset(pt->made, 1, pt->ntri);
    }
}

/* The grass's ground from ntri triangles of the current draw (their corners' indices idx among v), as render_triangle
 * gives it the CPU's (lit beforehand: the vertices' own colours) */
static void tl_lay_grass(const TlVertex *v, const uint32_t *idx, uint32_t ntri) {
    static float (*cw)[3];
    static size_t cap;
    if (3 * (size_t)ntri > cap) {
        cap = 6 * (size_t)ntri;
        cw = realloc(cw, cap * sizeof *cw);
    }
    const TlDraw *d = &tl_last;
    TlPart pt = {v, idx, ntri, 0, d, NULL};
    tl_corners(&pt, cw);
    int tex = (d->flags & TL_TEX) != 0;
    for (uint32_t t = 0; t < ntri; t++) {
        RVertex r[3];
        memset(r, 0, sizeof r);
        for (int k = 0; k < 3; k++) {
            const TlVertex *x = &v[idx[3 * t + (uint32_t)k]];
            memcpy(&r[k].wx, cw[3 * t + (uint32_t)k], sizeof cw[0]);
            r[k].u = tex ? x->uv[0] * d->tex[0] + d->tex[2] : 0;
            r[k].v = tex ? x->uv[1] * d->tex[1] + d->tex[3] : 0;
            r[k].r = (float)(x->color & 255);
            r[k].g = (float)((x->color >> 8) & 255);
            r[k].b = (float)((x->color >> 16) & 255);
        }
        grass_scenery(&cur, &r[0], &r[1], &r[2]);
    }
}

/* Whether the current draw's triangles lay the grass's ground (as render_triangle has them) */
static int tl_grass(void) {
    return grass_on && shaded(&cur) && ((cur.flags & DRAW_TFUNC) == 0 || (cur.flags & DRAW_TFUNC) == 3) &&
           grass_laying();
}

/* gpu.c: a primitive (triangles, strip, fan) whose vertex stage the GPU does - its draw (NULL: the last's), its vertices
 * as the game gave them, its count vertices' indices among them. Its triangles are indexed as gpu.c's clipping takes
 * them (a strip's every other one turned round, to keep its winding) */
void render_tl_draw(int type, int count, const TlDraw *d, const TlVertex *v, int nv, const uint16_t *idx,
                    const float *feet, int nfeet) {
    uint32_t rec = tl_pipeline ? tl_record(d) : ~0u;
    int ntri = type == 3 ? count / 3 : count - 2;
    if (ntri <= 0 || rec == ~0u)
        return;
    tl_begin();
    if (tl_nvert + (uint32_t)nv > TL_VERTICES || tl_nidx + 3u * (uint32_t)ntri > TL_INDICES) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "renderer: the GPU vertex stage's vertices a frame run out\n");
        return;
    }
    uint32_t base = (uint32_t)vk.frame * TL_VERTICES + tl_nvert;
    memcpy((TlVertex *)tl_vertices.map + base, v, sizeof *v * (size_t)nv);
    tl_nvert += (uint32_t)nv;
    uint32_t first = (uint32_t)vk.frame * TL_INDICES + tl_nidx;
    uint32_t *o = (uint32_t *)tl_indices.map + first;   /* (write-combined: written in order, never read) */
    for (int t = 0; t < ntri; t++) {
        int a, b, c;
        if (type == 3)
            a = 3 * t, b = 3 * t + 1, c = 3 * t + 2;
        else if (type == 4)
            a = (t & 1) ? t + 1 : t, b = (t & 1) ? t : t + 1, c = t + 2;
        else
            a = 0, b = t + 1, c = t + 2;
        *o++ = base + idx[a];
        *o++ = base + idx[b];
        *o++ = base + idx[c];
    }
    tl_nidx += 3u * (uint32_t)ntri;
    int effects = shaded(&cur) || swaying(&cur), grass = tl_grass();
    if ((effects || grass) && tl_nsent_i + 3u * (uint32_t)ntri <= TL_INDICES) {
        /* (the scenery's effects: the vertices kept, its triangles' corners as they were indexed) */
        uint32_t *tri = tl_sent_i + tl_nsent_i, local = base - (uint32_t)vk.frame * TL_VERTICES;
        memcpy(tl_sent_v + local, v, sizeof *v * (size_t)nv);
        for (int t = 0; t < ntri; t++) {
            int a, b, c;
            if (type == 3)
                a = 3 * t, b = 3 * t + 1, c = 3 * t + 2;
            else if (type == 4)
                a = (t & 1) ? t + 1 : t, b = (t & 1) ? t : t + 1, c = t + 2;
            else
                a = 0, b = t + 1, c = t + 2;
            tri[3 * t] = idx[a];
            tri[3 * t + 1] = idx[b];
            tri[3 * t + 2] = idx[c];
        }
        tl_nsent_i += 3u * (uint32_t)ntri;
        if (grass)
            tl_lay_grass(tl_sent_v + local, tri, (uint32_t)ntri);
        if (effects)
            tl_part(tl_sent_v + local, tri, (uint32_t)ntri, base, NULL);
    }
    tl_segment(first, 3u * (uint32_t)ntri, rec);
    tl_feet(feet, nfeet);
}

/* gpu.c, as a frame is recorded: vertices and triangles' indices (among the slot's vertices) into a slot, at vertex
 * vfirst and index ifirst. A slot starts again from 0 for each frame recorded */
void render_tl_store(int slot, uint32_t vfirst, const TlVertex *v, int nv, uint32_t ifirst, const uint32_t *idx, int ni) {
    if (!tl_pipeline || vfirst + (uint32_t)nv > TL_SLOT_VERTICES || ifirst + (uint32_t)ni > TL_SLOT_INDICES)
        return;
    if (!vfirst && !ifirst && tl_slot_use[slot] && tl_slot_use[slot] >= tl_submits) {
        /* (a slot is four recorded frames old; drawn from in the submission still running: waited for) */
        if (tl_slot_use[slot] == tl_submits) {
            vk_finish();
        } else {
            static int warned;
            if (!warned++)
                fprintf(stderr, "renderer: a slot of the GPU vertex stage written while it is to be drawn from\n");
        }
    }
    uint32_t vbase = 2 * TL_VERTICES + (uint32_t)slot * TL_SLOT_VERTICES;
    memcpy((TlVertex *)tl_vertices.map + vbase + vfirst, v, sizeof *v * (size_t)nv);
    uint32_t *o = (uint32_t *)tl_indices.map + 2 * TL_INDICES + (uint32_t)slot * TL_SLOT_INDICES + ifirst;
    for (int i = 0; i < ni; i++)
        o[i] = vbase + idx[i];
    memcpy(tl_slot_v[slot] + vfirst, v, sizeof *v * (size_t)nv);
    memcpy(tl_slot_i[slot] + ifirst, idx, sizeof *idx * (size_t)ni);
    memset(tl_slot_made[slot] + ifirst / 3, 0, (size_t)ni / 3);
}

/* gpu.c, replaying a recorded frame: n of a slot's indices from first, with a draw (NULL: the last's) */
void render_tl_slot_draw(int slot, const TlDraw *d, uint32_t first, uint32_t n, const float *feet, int nfeet) {
    uint32_t rec = tl_pipeline ? tl_record(d) : ~0u;
    if (!n || rec == ~0u || first + n > TL_SLOT_INDICES)
        return;
    tl_begin();
    tl_slot_use[slot] = tl_submits + 1;
    /* the scenery's effects (made as the batch is flushed), the grass's ground each frame laying it */
    if (shaded(&cur) || swaying(&cur))
        tl_part(tl_slot_v[slot], tl_slot_i[slot] + first, n / 3, 2 * TL_VERTICES + (uint32_t)slot * TL_SLOT_VERTICES,
                tl_slot_made[slot] + first / 3);
    if (tl_grass())
        tl_lay_grass(tl_slot_v[slot], tl_slot_i[slot] + first, n / 3);
    tl_segment(2 * TL_INDICES + (uint32_t)slot * TL_SLOT_INDICES + first, n, rec);
    tl_feet(feet, nfeet);
}

void render_vram_written(uint32_t addr, uint32_t size) {
    (void)addr; (void)size;     /* TODO: upload into a render target the region covers */
}

/* -- frames ------------------------------------------------------------------------------------------------------- */

/* The anti-aliasing's offset of the world this frame, render target pixels: eight places in turn (Halton 2, 3), none
 * without it */
float render_jitter[2];

static float halton(int i, int b) {
    float f = 1, r = 0;
    for (; i > 0; i /= b) {
        f /= (float)b;
        r += f * (float)(i % b);
    }
    return r;
}

static void next_jitter(void) {
    static int on = -1;
    if (on < 0)                     /* (MHFU_TAA_JITTER=0: none, for checking the anti-aliasing's reprojection) */
        on = !getenv("MHFU_TAA_JITTER") || atoi(getenv("MHFU_TAA_JITTER")) != 0;
    int i = (int)(frame_no % 8) + 1;
    render_jitter[0] = post_taa() && on ? halton(i, 2) - 0.5f : 0.0f;
    render_jitter[1] = post_taa() && on ? halton(i, 3) - 0.5f : 0.0f;
}

static void end_frame(void) {
    ao_end_frame();
    post_frame_done();
    scene_end_frame();
    shadow_end_frame(lights_end_frame(cur.tex_slot));
    vertex_used = batch_start = 0;
    tl_nrec = tl_nvert = tl_nidx = tl_nsent_i = 0;
    tl_rec = ~0u;
    staging_used = 0;
    last_valid = 0;
    frame_no++;
    next_jitter();
}

/* The end of a frame not shown (fast-forwarding): its work submitted, the frame's buffers and checks started again */
void render_finish_frame(void) {
    render_queue_sync();                /* (called from outside: the renderer's own again) */
    ao_finish();
    end_rendering();
    if (vk.recording)
        vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    end_frame();
}

/* The picture onto an image of w x h being drawn into (the window's): letterboxed to its shape (480:272, or the
 * little wider one it is drawn at to fill a screen: render_wide); its own size there when
 * that is within a pixel of what fits (as the window's when the internal resolution follows it), at a whole pixel */
static void picture_place(uint32_t w, uint32_t h, float *x0, float *y0, float *vw_out, float *vh_out) {
    float sw = (float)w, sh = (float)h, src[2] = {(float)scaled_x(480), (float)scaled(272)};
    float shape = 480.0f * render_wide / 272.0f;
    float vw = sw, vh = sw / shape;
    if (vh > sh) {
        vh = sh;
        vw = sh * shape;
    }
    if (fabsf(vw - src[0]) < 1.5f && fabsf(vh - src[1]) < 1.5f) {
        vw = src[0];
        vh = src[1];
    } else {
        vw = roundf(vw);
        vh = roundf(vh);
    }
    *x0 = floorf((sw - vw) / 2);
    *y0 = floorf((sh - vh) / 2);
    *vw_out = vw;
    *vh_out = vh;
}

static void present_draw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, uint32_t w, uint32_t h) {
    float x0, y0, vw, vh;
    picture_place(w, h, &x0, &y0, &vw, &vh);
    float pc[6] = {(float)scaled_x(480), (float)scaled(272), x0, y0, vw, vh};
    VkViewport vp = {pc[2], pc[3], vw, vh, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &set, 0, NULL);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, present_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

/* A note on the screen for a moment (a save state saved, loaded...): capitals, figures and a few signs, in a 5 x 7
 * font, light on a dark box at the top - drawn into the frame shown, as cleared rectangles */
static char note_text[64];
static double note_until;

uint64_t wall_time_us(void);         /* (thread.c) */

static double note_clock(void) {
    return (double)wall_time_us() / 1e6;
}

void render_note(const char *text) {
    size_t n = 0;
    for (; text[n] && n + 1 < sizeof note_text; n++)
        note_text[n] = text[n] >= 'a' && text[n] <= 'z' ? (char)(text[n] - 32) : text[n];
    note_text[n] = 0;
    note_until = note_clock() + 2.5;
}

static void draw_note(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    if (!note_text[0] || note_clock() > note_until)
        return;
    int dot = (int)h / 240 > 2 ? (int)h / 240 : 2, n = (int)strlen(note_text);
    int width = n * 6 * dot + 3 * dot, height = 13 * dot;
    int x0 = ((int)w - width) / 2, y0 = 4 * dot;
    static VkClearRect rects[64 * 35 + 1];
    int nr = 0;
    rects[nr++] = (VkClearRect){{{x0, y0}, {(uint32_t)width, (uint32_t)height}}, 0, 1};
    VkClearAttachment box = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{0.05f, 0.05f, 0.06f, 1}}}};
    vkCmdClearAttachments(cmd, 1, &box, 1, rects);
    nr = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = glyph_font(note_text[i]);
        for (int row = 0; g && row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] >> (4 - col) & 1)
                    rects[nr++] = (VkClearRect){{{x0 + 2 * dot + (i * 6 + col) * dot, y0 + 3 * dot + row * dot},
                                                 {(uint32_t)dot, (uint32_t)dot}}, 0, 1};
    }
    VkClearAttachment ink = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{0.92f, 0.9f, 0.82f, 1}}}};
    if (nr)
        vkCmdClearAttachments(cmd, 1, &ink, (uint32_t)nr, rects);
}

/* The button hints (glyphs.c): the keys and buttons bound, over the PSP's buttons' places in the picture - and the
 * controls screen (overlay.c) over all */
int overlay_rects(float x0, float y0, float scale, HintRect *out, int max);
int inspect_rects(float x0, float y0, float scale, HintRect *out, int max);
int hit_view_rects(float x0, float y0, float scale, HintRect *out, int max);
int ticket_rects(float x0, float y0, float scale, HintRect *out, int max);
#define MAX_HINT_RECTS 16384
static void draw_hints(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    {
        int camera_free_hud_off(void);      /* (a picture with nothing over it) */
        if (camera_free_hud_off())
            return;
    }
    static HintRect rects[MAX_HINT_RECTS];
    static VkClearRect by_color[MAX_HINT_RECTS];
    float x0, y0, vw, vh;
    picture_place(w, h, &x0, &y0, &vw, &vh);
    x0 += (vw - vh * 480.0f / 272.0f) / 2;      /* (the PSP's shape, in the middle of a picture a little wider) */
    vw = vh * 480.0f / 272.0f;
    int n = glyphs_rects(x0, y0, vw / 480.0f, rects, MAX_HINT_RECTS);
    n += perf_rects(x0, y0, vw / 480.0f, rects + n, MAX_HINT_RECTS - n);      /* (the performance panel) */
    n += inspect_rects(x0, y0, vw / 480.0f, rects + n, MAX_HINT_RECTS - n);   /* (the click inspector) */
    n += hit_view_rects(x0, y0, vw / 480.0f, rects + n, MAX_HINT_RECTS - n);  /* (the hit overlay) */
    n += ticket_rects(x0, y0, vw / 480.0f, rects + n, MAX_HINT_RECTS - n);    /* (the ticket's note box) */
    n += overlay_rects(x0, y0, vw / 480.0f, rects + n, MAX_HINT_RECTS - n);    /* (the controls screen over them) */
    for (int i = 0; i < n;) {           /* (in order, each run of one colour cleared at once) */
        int c = rects[i].color;
        uint32_t k = 0;
        for (; i < n && rects[i].color == c; i++) {
            const HintRect *r = &rects[i];
            int x = r->x < 0 ? 0 : r->x, y = r->y < 0 ? 0 : r->y;
            int x1 = r->x + r->w > (int)w ? (int)w : r->x + r->w, y1 = r->y + r->h > (int)h ? (int)h : r->y + r->h;
            if (x1 > x && y1 > y)
                by_color[k++] = (VkClearRect){{{x, y}, {(uint32_t)(x1 - x), (uint32_t)(y1 - y)}}, 0, 1};
        }
        if (!k)
            continue;
        VkClearAttachment ink = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{hint_palette[c][0], hint_palette[c][1],
                                                                  hint_palette[c][2], hint_palette[c][3]}}}};
        vkCmdClearAttachments(cmd, 1, &ink, k, by_color);
    }
}

void render_flip(uint32_t addr) {
    render_queue_sync();                /* (called from outside: the renderer's own again) */
    ao_finish();
    post_finish();
    end_rendering();
    Target *t = target_at(addr, 0);
    if (!vk.surface || !t) {
        if (vk.recording)
            vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        end_frame();
        if (resolution_changed)
            resolution_set();
        /* MHFU_TEST_RESCALE=frame,scale: as if the window were resized there (headless) */
        static int rescaled;
        unsigned long at;
        float to;
        if (!rescaled && getenv("MHFU_TEST_RESCALE") &&
            sscanf(getenv("MHFU_TEST_RESCALE"), "%lu,%f", &at, &to) == 2 && frame_no >= at) {
            rescaled = 1;
            render_rescale(to, 1);
        }
        return;
    }
    uint32_t index;
    uint64_t present_at = perf_on ? perf_ns() : 0;
    VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired, VK_NULL_HANDLE, &index);
    if (present_at)
        perf_present_ns += perf_ns() - present_at;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        end_frame();
        window_changed();
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
    present_draw(cmd, present_pipeline, present_sets[vk.frame], vk.swap_extent.width, vk.swap_extent.height);
    draw_hints(cmd, vk.swap_extent.width, vk.swap_extent.height);
    draw_note(cmd, vk.swap_extent.width, vk.swap_extent.height);
    vkCmdEndRendering(cmd);
    vk_transition(&swap, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vk_submit(vk.acquired, vk.rendered);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, pacing_present_chain(), 1, &vk.rendered, 1, &vk.swapchain,
                           &index, NULL};
    present_at = perf_on ? perf_ns() : 0;
    r = vkQueuePresentKHR(vk.queue, &pi);
    if (present_at)
        perf_present_ns += perf_ns() - present_at;
    pacing_presented(r);
    end_frame();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || window_resized)
        window_changed();
    if (resolution_changed)
        resolution_set();
}

void render_sun_strength_set(float s) {
    render_sun_strength = s;            /* (a float, read by the renderer's thread as it is) */
}

/* How much wider than the PSP's shape the picture is (window.c: where the pointer is on it) */
float render_picture_wide(void) {
    return render_wide;
}

/* The size frames are shown at: the window's, or headless the internal resolution */
void render_shown_size(int *w, int *h) {
    render_queue_sync();                /* (called from outside: the renderer's own again) */
    if (vk.surface) {
        *w = (int)vk.swap_extent.width;
        *h = (int)vk.swap_extent.height;
    } else {
        *w = (int)scaled_x(480);
        *h = (int)scaled(272);
    }
}

/* The displayed part of a framebuffer at the internal resolution (MHFU_DUMP_FULL): malloc'd, *w by *h */
uint32_t *render_readback_full(uint32_t addr, int *w_out, int *h_out) {
    render_queue_sync();                /* (called from outside: the renderer's own again) */
    ao_finish();
    post_finish();
    end_rendering();
    Target *t = target_at(addr, 0);
    if (!t)
        return NULL;
    vk_transition(&t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uint32_t w = scaled_x(480), h = scaled(272);
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

/* The displayed part of a framebuffer as the window shows it at w x h (MHFU_DUMP_PRESENT): malloc'd */
uint32_t *render_readback_present(uint32_t addr, int w, int h) {
    render_queue_sync();                /* (called from outside: the renderer's own again) */
    static VkPipeline pipeline;
    ao_finish();
    post_finish();
    end_rendering();
    Target *t = target_at(addr, 0);
    if (!t)
        return NULL;
    if (!pipeline)
        pipeline = make_pipeline(vk_shader(present_vert, sizeof present_vert),
                                 vk_shader(present_frag, sizeof present_frag), present_layout,
                                 VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, 0);
    VkImg out = vk_image((uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBuf buf = vk_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    VkCommandBuffer cmd = vk_cmd();
    vk_transition(&t->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii = {present_sampler, t->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, present_sets[vk.frame], 0, 0, 1,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &wr, 0, NULL);
    vk_transition(&out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, out.view,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{{0, 0, 0, 1}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {(uint32_t)w, (uint32_t)h}}, 1, 0, 1,
                          &ca, NULL, NULL};
    vkCmdBeginRendering(cmd, &ri);
    present_draw(cmd, pipeline, present_sets[vk.frame], (uint32_t)w, (uint32_t)h);
    draw_hints(cmd, (uint32_t)w, (uint32_t)h);
    vkCmdEndRendering(cmd);
    vk_transition(&out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyImageToBuffer(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &copy);
    vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_finish();
    uint32_t *p = malloc((size_t)w * h * 4);
    memcpy(p, buf.map, (size_t)w * h * 4);
    vk_image_free(&out);
    vk_buffer_free(&buf);
    return p;
}

/* The displayed part of a framebuffer, for a frame dump: read back, and scaled down to 480 x 272 */
void render_readback(uint32_t addr, uint32_t *pixels) {
    render_queue_sync();                /* (called from outside: the renderer's own again) */
    ao_finish();
    post_finish();
    end_rendering();
    Target *t = target_at(addr, 0);
    if (!t) {
        memset(pixels, 0, 480 * 272 * 4);
        return;
    }
    vk_transition(&t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uint32_t w = scaled_x(480), h = scaled(272);
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(vk_cmd(), t->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_finish();
    const uint32_t *src = readback.map;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            pixels[y * 480 + x] = src[(uint32_t)(y * render_scale) * w + (uint32_t)(x * render_scale * render_wide)] | 0xFF000000u;
}
