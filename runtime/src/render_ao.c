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

#define AO_FADE 6000.0f                 /* gone by this far */
#define AO_PER_FRAME 8                  /* times a frame (the frame dumps draw it again) */
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
    for (int i = 0; i < 2; i++)
        ao_img[i] = vk_image(scaled(480) / 2, scaled(272) / 2, VK_FORMAT_R16G16_SFLOAT,
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

int ao_ready(void) {
    return ao_fresh;
}

VkImageView ao_image(void) {
    return ao_on > 0 ? ao_img[0].view : VK_NULL_HANDLE;
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
}
