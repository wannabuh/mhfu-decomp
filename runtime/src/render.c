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
#include "render_internal.h"

#include "grass.h"
#include "hle.h"
#include "post.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "ge.vert.inc"
#include "ge.frag.inc"
#include "ge.tesc.inc"
#include "ge.tese.inc"
#include "present.vert.inc"
#include "present.frag.inc"

int render_on;
float render_scale = 2;
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
static VkShaderModule tess_ctrl, tess_eval;
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
 * full-screen pass multiplied into what is there, 5 one added to it, 6 the GE's tessellated, 7 the grass's blades */
VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout pl, VkFormat color, VkFormat depth,
                         int ge) {
    VkPipelineShaderStageCreateInfo stages[4] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
         tess_ctrl, "main", NULL},
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
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        ge == 6 ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, NULL, 0, 3};
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

/* window.c: the internal resolution to use unless MHFU_SCALE says (enough for the screen's lines) */
void render_default_scale(float s) {
    if (s >= 1 && s <= 12)
        render_scale = s;
}

int render_init(void *sdl_window) {
    /* the internal resolution: MHFU_RESOLUTION=WxH (its lines, or as wide as it is if that is less: 2560x1440 is 5.29
     * times the PSP's), or MHFU_SCALE times the PSP's (fractions too) - 1 to 12 times */
    const char *s = getenv("MHFU_SCALE"), *res = getenv("MHFU_RESOLUTION");
    int rw, rh;
    if (res && sscanf(res, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
        float r = fminf((float)rw / 480.0f, (float)rh / 272.0f);
        if (r >= 1 && r <= 12)
            render_scale = r;
    } else if (s && atof(s) >= 1 && atof(s) <= 12)
        render_scale = (float)atof(s);
    if (!vk_init(sdl_window))
        return 0;

    tess_on = vk.tessellation && (!getenv("MHFU_TESSELLATION") || atoi(getenv("MHFU_TESSELLATION")) != 0);
    if (tess_on)
        pc_stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    /* the GE's descriptor set (shared.h's bindings): the textures bindless, an array filled as they are made */
    VkDescriptorSetLayoutBinding b[6] = {
        {BIND_TEXTURES, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {BIND_SHADOW_MAP, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {BIND_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, pc_stages, NULL},
        {BIND_POINT_SHADOWS, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {BIND_TRAIL, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {BIND_AO, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
    };
    VkDescriptorBindingFlags bf[6];
    for (int i = 0; i < 6; i++)
        bf[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bf[BIND_TEXTURES] |= VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 6, bf};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi,
                                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 6, b};
    vkCreateDescriptorSetLayout(vk.device, &dl, NULL, &set_layout);
    VkDescriptorSetLayoutBinding pb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       NULL};
    VkDescriptorSetLayoutCreateInfo pdl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &pb};
    vkCreateDescriptorSetLayout(vk.device, &pdl, NULL, &present_set_layout);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES + 6},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
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
    VkPushConstantRange ppr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
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
    readback = vk_buffer((VkDeviceSize)scaled(RT_SIZE) * scaled(RT_SIZE) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);

    /* the parts (render_internal.h) */
    texture_init();
    shadow_init();
    lights_init();
    ao_init();
    post_setup(vs, fs, tess_pipeline != VK_NULL_HANDLE);
    grass_init();
    vk.frame_reused = frame_reused;
    render_on = 1;
    fprintf(stderr, "renderer: Vulkan, %ux%u internal resolution (%.3g times the PSP's)\n", scaled(480), scaled(272),
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
            uint32_t px = scaled(RT_SIZE);
            t->used = 1;
            t->addr = addr;
            t->color = vk_image(px, px, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
            t->depth = vk_image(px, px, VK_FORMAT_D32_SFLOAT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
            t->sample_slot = texture_new_slot();
            write_slot(t->sample_slot, t->color.view, samplers[1 | 2 | 4]);
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
    uint32_t px = scaled(RT_SIZE);
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {px, px}}, 1, 0, 1, &ca, &da, NULL};
    VkCommandBuffer cmd = vk_cmd();
    vkCmdBeginRendering(cmd, &ri);
    t->cleared = 1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_pipeline);
    bound_pipeline = ge_pipeline;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_layout, 0, 1, &ge_set, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &zero);
    VkViewport vp = {0, 0, (float)px, (float)px, 0, 1};
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
    s.scissor = (VkRect2D){{(int32_t)scaled(sx0), (int32_t)scaled(sy0)}, {scaled(sx1) - scaled(sx0), scaled(sy1) - scaled(sy0)}};
    /* the solid world drawn, then what is not: its occlusion (and the grass), then post-processing; its fog and depth
     * test noted (the grass's) */
    int solid = (s.flags & DRAW_WORLD) && s.depth_write;
    ao_state(s.target, solid, clear);
    if (solid) {
        scene.depth_op = s.depth_op;
        uint32_t fe = reg[0xCD] << 8, fs = reg[0xCE] << 8;
        memcpy(&scene.fog[0], &fe, 4);
        memcpy(&scene.fog[1], &fs, 4);
        scene.fog[2] = (s.flags & DRAW_FOG) ? 1.0f : 0.0f;
        scene.fog_color = s.fog_color;
    }
    post_state(s.target, s.flags, solid, clear);
    if (memcmp(&s, &cur, sizeof s)) {
        flush();
        cur = s;
    }
    static int settings_read;
    if (!settings_read) {
        settings_read = 1;
        lights_settings();
        shadow_settings();
        shading_settings();
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

/* Draw what has been batched with the current state */
void flush(void) {
    VkDeviceSize n = (vertex_used - batch_start) / sizeof(GpuVertex);
    if (!n || !rendering) {
        shadow_skip();
        return;
    }
    if (shaded(&cur))
        make_normals((size_t)n);
    if (swaying(&cur))
        sway_batch(&cur, (size_t)n);
    if (finds_feet(&cur))
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
    put(a, a->x, a->y, a->u, a->v, 0);
    put(b, b->x, b->y, b->u, b->v, 0);
    put(c, c->x, c->y, c->u, c->v, 0);
    if (grass_on && shaded(&cur) && ((cur.flags & DRAW_TFUNC) == 0 || (cur.flags & DRAW_TFUNC) == 3))
        grass_scenery(&cur, a, b, c);
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
    ao_end_frame();
    post_frame_done();
    scene_end_frame();
    shadow_end_frame(lights_end_frame(cur.tex_slot));
    vertex_used = batch_start = 0;
    staging_used = 0;
    last_valid = 0;
    frame_no++;
}

/* The end of a frame not shown (fast-forwarding): its work submitted, the frame's buffers and checks started again */
void render_finish_frame(void) {
    ao_finish();
    end_rendering();
    if (vk.recording)
        vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    end_frame();
}

void render_flip(uint32_t addr) {
    ao_finish();
    post_finish();
    end_rendering();
    Target *t = target_at(addr, 0);
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
    ao_finish();
    post_finish();
    end_rendering();
    Target *t = target_at(addr, 0);
    if (!t)
        return NULL;
    vk_transition(&t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uint32_t w = scaled(480), h = scaled(272);
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
    ao_finish();
    post_finish();
    end_rendering();
    Target *t = target_at(addr, 0);
    if (!t) {
        memset(pixels, 0, 480 * 272 * 4);
        return;
    }
    vk_transition(&t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uint32_t w = scaled(480), h = scaled(272);
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(vk_cmd(), t->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    vk_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_finish();
    const uint32_t *src = readback.map;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            pixels[y * 480 + x] = src[(uint32_t)(y * render_scale) * w + (uint32_t)(x * render_scale)] | 0xFF000000u;
}
