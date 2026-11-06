/* Ambient occlusion (render_internal.h): from the depth of the world once it is drawn (before what is drawn over it -
 * the HUD), at half resolution, blurred, multiplied into the picture (MHFU_AO=0: none; MHFU_AO_STRENGTH,
 * MHFU_AO_RADIUS). Then the ground grass is drawn (render_grass.c: after it, a field of blades being all crevices to the
 * occlusion, which darkened it far below its ground) - it takes the occlusion of the ground it grows from. */
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

#include "present.vert.inc"
#include "ao.frag.inc"
#include "ao_blur.frag.inc"
#include "ao_apply.frag.inc"
#include "ao_water.frag.inc"

#define AO_FADE 6000.0f                 /* gone by this far */
#define AO_PER_FRAME 8                  /* times a frame (the frame dumps draw it again) */
#define MAX_WATER_DRAWS 512
#define MAX_WATER_SEGMENTS 2048
static int ao_on = -1;
static float ao_strength = 2.0f, ao_radius = 120.0f;
static VkImg ao_img[2];
static VkPipeline ao_pipeline, ao_blur_pipeline, ao_apply_pipeline;
static VkPipelineLayout ao_layout;
static VkDescriptorSetLayout ao_set_layout;
static VkDescriptorPool ao_pool;
static VkDescriptorSet ao_sets[2][AO_PER_FRAME][4];     /* a frame in flight's: the pass, the two blurs, the picture */
static VkSampler depth_sampler;
static Target *ao_target;               /* the solid world was drawn into it, without occlusion yet */
static Target *ao_applied;              /* it has its occlusion (until it is cleared) */
static int ao_used;                     /* times this frame */
static int ao_fresh;                    /* ao_img[0] holds the occlusion of what is being drawn (for the grass) */
/* What shows of the water and the foam on it (light added by flat planes, also writing depth): their depth is no
 * surface, so the passes need telling which pixels they are. Their draws are kept and drawn again against the finished
 * depth, in front of them what came later (kept: first, count its vertices; tl: its segments in water_segs) */
static VkImg ao_water;
static VkPipeline water_pipeline, water_tl_pipeline;
static struct { DrawState s; DrawPush pc; uint32_t first, count; int tl; } water_draws[MAX_WATER_DRAWS];
static int nwater;
static TlSegment water_segs[MAX_WATER_SEGMENTS];
static int nwater_segs;

/* Its images, at half the internal resolution (again when that changes: render_rescale, the GPU idle) */
void ao_rescale(void) {
    if (ao_on <= 0)
        return;
    for (int i = 0; i < 2; i++) {
        vk_image_free(&ao_img[i]);
        ao_img[i] = vk_image(scaled(480) / 2, scaled(272) / 2, VK_FORMAT_R16G16_SFLOAT,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    vk_image_free(&ao_water);
    ao_water = vk_image(scaled(480), scaled(272), VK_FORMAT_R8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    ao_fresh = 0;
}

/* Ambient occlusion's images, pipelines and descriptor sets (each pass's: its input, and the depth) */
void ao_init(void) {
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
    ao_rescale();
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, b};
    vkCreateDescriptorSetLayout(vk.device, &dl, NULL, &ao_set_layout);
    enum { SETS = 2 * AO_PER_FRAME * 4 };
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * SETS};
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

/* The pipelines drawing the water again (the GE's vertex shader vs; tl_vs: the GPU's vertex stage's, or none) */
void ao_water_setup(VkShaderModule vs, VkShaderModule tl_vs) {
    if (ao_on <= 0)
        return;
    VkShaderModule fs = vk_shader(ao_water_frag, sizeof ao_water_frag);
    water_pipeline = make_pipeline(vs, fs, ge_layout, VK_FORMAT_R8_UNORM, VK_FORMAT_D32_SFLOAT, 1);
    if (tl_vs)
        water_tl_pipeline = make_pipeline(tl_vs, fs, ge_layout, VK_FORMAT_R8_UNORM, VK_FORMAT_D32_SFLOAT, 8);
}

/* A batch drawn into t: kept if it is water or foam writing depth, to be marked before the occlusion (seg: the GPU's
 * vertex stage's segments, or NULL and its vertices first, n) */
void ao_keep(Target *t, const DrawState *s, const DrawPush *pc, uint32_t first, uint32_t n, const TlSegment *seg) {
    int adds = (s->flags & DRAW_WORLD) && s->blend && s->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE &&
               s->eq.colorBlendOp == VK_BLEND_OP_ADD;
    if (t != ao_target || !((s->flags & DRAW_WATER) || adds) || !s->depth_write || nwater == MAX_WATER_DRAWS)
        return;
    if (seg) {
        if (nwater_segs + (int)n > MAX_WATER_SEGMENTS)
            return;
        memcpy(&water_segs[nwater_segs], seg, sizeof *seg * n);
        first = (uint32_t)nwater_segs;
        nwater_segs += (int)n;
    }
    water_draws[nwater].s = *s;
    water_draws[nwater].pc = *pc;
    water_draws[nwater].first = first;
    water_draws[nwater].count = n;
    water_draws[nwater++].tl = seg != NULL;
}

/* The water kept drawn again into the mask, where it is what shows */
static void water_mask(Target *t) {
    uint32_t fw = scaled(480), fh = scaled(272), px = scaled(RT_SIZE);
    VkCommandBuffer cmd = vk_cmd();
    vk_transition(&t->depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    vk_transition(&ao_water, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, ao_water.view,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, t->depth.view,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {fw, fh}}, 1, 0, 1, &ca, &da, NULL};
    vkCmdBeginRendering(cmd, &ri);
    if (nwater && water_pipeline) {
        VkPipeline bound = water_pipeline;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_layout, 0, 1, &ge_set, 0, NULL);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &zero);
        VkViewport vp = {0, 0, (float)px, (float)px, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
        VkBool32 off = VK_FALSE;
        vk.SetColorBlendEnable(cmd, 0, 1, &off);
        VkColorComponentFlags all = 0xF;
        vk.SetColorWriteMask(cmd, 0, 1, &all);
        for (int i = 0; i < nwater; i++) {
            const DrawState *d = &water_draws[i].s;
            VkPipeline p = water_draws[i].tl ? water_tl_pipeline : water_pipeline;
            VkRect2D sc = d->scissor;
            if (!p || sc.offset.x >= (int32_t)fw || sc.offset.y >= (int32_t)fh)
                continue;
            if (p != bound)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            if (sc.offset.x + sc.extent.width > fw)
                sc.extent.width = fw - (uint32_t)sc.offset.x;
            if (sc.offset.y + sc.extent.height > fh)
                sc.extent.height = fh - (uint32_t)sc.offset.y;
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdSetDepthTestEnable(cmd, d->depth_test);
            VkCompareOp op = d->depth_op;       /* (its own depth: drawn again, it passes) */
            if (op == VK_COMPARE_OP_GREATER)
                op = VK_COMPARE_OP_GREATER_OR_EQUAL;
            if (op == VK_COMPARE_OP_LESS)
                op = VK_COMPARE_OP_LESS_OR_EQUAL;
            vkCmdSetDepthCompareOp(cmd, op);
            vk.SetColorBlendEquation(cmd, 0, 1, &d->eq);
            vkCmdSetBlendConstants(cmd, d->blend_const);
            vkCmdSetCullMode(cmd, d->cull);
            vkCmdSetFrontFace(cmd, d->front);
            DrawPush dp = water_draws[i].pc;
            if (!water_under(t))                /* (nothing to see under foam: as water) */
                dp.flags |= DRAW_WATER;
            vkCmdPushConstants(cmd, ge_layout, pc_stages, 0, sizeof dp, &dp);
            if (water_draws[i].tl) {
                vkCmdBindVertexBuffers(cmd, 1, 1, &tl_vertices.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, tl_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                for (uint32_t k = water_draws[i].first; k < water_draws[i].first + water_draws[i].count; k++)
                    vkCmdDrawIndexed(cmd, water_segs[k].count, 1, water_segs[k].first, 0, water_segs[k].rec);
            } else {
                vkCmdDraw(cmd, water_draws[i].count, 1, water_draws[i].first, 0);
            }
        }
    }
    vkCmdEndRendering(cmd);
    vk_transition(&ao_water, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    nwater = nwater_segs = 0;
    ge_lost();
}

int ao_ready(void) {
    return ao_fresh;
}

VkImageView ao_image(void) {
    return ao_on > 0 ? ao_img[0].view : VK_NULL_HANDLE;
}

static void ao_set(VkDescriptorSet ds, VkImageView source, VkImageLayout layout, VkSampler sampler, VkImageView depth,
                   VkImageView under) {
    VkDescriptorImageInfo ii[4] = {{sampler, source, layout},
                                   {depth_sampler, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                   {depth_sampler, ao_water.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {depth_sampler, under, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[4] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0],
         NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[1],
         NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[2],
         NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[3],
         NULL, NULL}};
    vkUpdateDescriptorSets(vk.device, 4, w, 0, NULL);
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
static void ao_occlude(Target *t);

/* The solid world drawn: its ambient occlusion, then the ground grass (after it: a field of blades is all crevices to
 * the occlusion, which darkened it far below its ground) */
static void ao_apply(Target *t) {
    ao_target = NULL;
    ao_applied = t;
    ao_fresh = 0;
    ao_occlude(t);
    grass_draw(t);
}

static void ao_occlude(Target *t) {
    if (ao_on <= 0 || ao_used >= AO_PER_FRAME)
        return;
    AoPush pc;
    if (!invert4(scene.view, pc.to_view))
        return;
    end_rendering();
    water_mask(t);
    uint32_t fw = scaled(480), fh = scaled(272), hw = fw / 2, hh = fh / 2;
    float focal = fabsf(scene.view[0]) * (float)render_scale;
    memcpy(pc.params, (float[4]){ao_radius, ao_strength, (float)render_scale, focal}, sizeof pc.params);
    memcpy(pc.params2, (float[4]){AO_FADE, 0, 0, 0}, sizeof pc.params2);
    memcpy(pc.area, (float[4]){(float)fw, (float)fh, (float)hw, (float)hh}, sizeof pc.area);
    /* the point lights (the first four), in the camera's space: where they shine, the occlusion takes nothing - it
     * takes away the light from all round, not theirs */
    memset(pc.lights, 0, sizeof pc.lights + sizeof pc.colors);
    float m4[16] = {0}, inv[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 3; i++)
            m4[4 * c + i] = scene.world_view[3 * c + i];
    m4[15] = 1;
    if (point_lights_on > 0 && invert4(m4, inv))
        for (uint32_t i = 0; i < lights_points()[0].count && i < 4; i++) {
            const float *w = lights_points()[0].dir[i];
            for (int k = 0; k < 3; k++)
                pc.lights[i][k] = inv[k] * w[0] + inv[4 + k] * w[1] + inv[8 + k] * w[2] + inv[12 + k];
            pc.lights[i][3] = w[3];
            memcpy(pc.colors[i], lights_points()[0].color[i], sizeof pc.colors[i]);
        }
    VkDescriptorSet *sets = ao_sets[vk.frame][ao_used++];
    VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageView under = water_under(t) ? water_under(t) : t->depth.view;
    ao_set(sets[0], t->depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, depth_sampler, t->depth.view, under);
    ao_set(sets[1], ao_img[0].view, ro, depth_sampler, t->depth.view, under);
    ao_set(sets[2], ao_img[1].view, ro, depth_sampler, t->depth.view, under);
    ao_set(sets[3], ao_img[0].view, ro, depth_sampler, t->depth.view, under);
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
    ao_fresh = 1;
}

/* A primitive drawn into t (solid: of the solid world). The solid world drawn, then what is not solid (glows, fire,
 * smoke, the HUD): the world's occlusion first - only the solid world's colours have it, once (until the picture is
 * cleared). The solid world's camera noted (scene.view) */
void ao_state(Target *t, int solid, int clear) {
    if (ao_target && t == ao_target && !solid) {
        flush();
        ao_apply(ao_target);
    }
    if (clear && t == ao_applied)
        ao_applied = NULL;
    if (clear && t == ao_target)
        nwater = nwater_segs = 0;
    if (solid && t != ao_applied) {
        ao_target = t;
        memcpy(scene.view, prim_view, sizeof scene.view);
        memcpy(scene.world_view, prim_world_view, sizeof scene.world_view);
    }
}

/* The frame shown or read: the occlusion still to apply */
void ao_finish(void) {
    if (ao_target)
        ao_apply(ao_target);
}

void ao_end_frame(void) {
    ao_target = ao_applied = NULL;
    ao_used = 0;
    nwater = nwater_segs = 0;
}
