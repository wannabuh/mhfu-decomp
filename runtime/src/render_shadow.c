/* Shadows (render_internal.h). The sun's: its depth over the world around the camera's target (gpu.c places it:
 * render_shadow_size), drawn each frame from what is drawn in the world - the same vertices, in a command buffer of its
 * own submitted before the frame's (MHFU_SHADOW_RESOLUTION; MHFU_SHADOWS=0 in gpu.c: none).
 *
 * The point lights' (render_lights.c): the two strongest lights' cube maps (6 faces each, distance from the light over
 * its reach), drawn in one pass from what casts shadows - each draw once into all twelve faces (instances; the vertex
 * shader picks the face's layer) - in a command buffer of their own before the frame's (MHFU_POINT_SHADOWS=0: none). */
#include "savestate.h"
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

#include "shadow.vert.inc"
#include "shadow.frag.inc"
#include "pshadow.vert.inc"
#include "pshadow.frag.inc"
#include "shadow_tl.vert.inc"
#include "pshadow_tl.vert.inc"

static int shadow_size = 4096;         /* MHFU_SHADOW_RESOLUTION */
#define SHADOW_SIZE ((uint32_t)shadow_size)
#define POINT_SHADOW_SIZE 512

int shadow_on, shadow_drawn;
VkImg shadow_img;
VkSampler shadow_sampler, point_sampler;
int point_shadows_on = 1;
static int point_shadows;           /* the point lights with shadows this frame (0 to 2) */
static VkPipeline shadow_pipeline, point_pipeline;
static VkPipeline shadow_tl_pipeline, point_tl_pipeline;    /* (the GE's vertex stage on the GPU: MHFU_GPU_TL) */
static VkCommandBuffer shadow_cmd, shadow_cmds[2], point_cmd, point_cmds[2];
static int shadow_recording, point_recording;
static VkImg point_img;
static VkImageView point_cube[2];
/* What clipping at the camera's near plane cut off (behind the camera, it still casts shadows): whole, for the shadow
 * pass only, in a buffer of their own (one a frame in flight) */
#define SHADOW_EXTRA_BYTES (8u << 20)
static VkBuf shadow_extra[2], extra;
static VkDeviceSize extra_used, extra_batch;

/* gpu.c: the shadow map's size, to place it */
int render_shadow_size(void) {
    return shadow_size;
}

static int shadow_finish(VkCommandBuffer *out);

void shadow_init(void) {
    if (getenv("MHFU_SHADOW_RESOLUTION")) {
        int r = atoi(getenv("MHFU_SHADOW_RESOLUTION"));
        if (r >= 512 && r <= 16384)
            shadow_size = r;
    }
    VkShaderModule sfs = vk_shader(shadow_frag, sizeof shadow_frag), pfs = vk_shader(pshadow_frag, sizeof pshadow_frag);
    shadow_pipeline = make_pipeline(vk_shader(shadow_vert, sizeof shadow_vert), sfs, ge_layout, VK_FORMAT_UNDEFINED,
                                    VK_FORMAT_D32_SFLOAT, 2);
    if (vk.output_layer)
        point_pipeline = make_pipeline(vk_shader(pshadow_vert, sizeof pshadow_vert), pfs, ge_layout,
                                       VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, 3);
    if (vk.clip_distance) {
        shadow_tl_pipeline = make_pipeline(vk_shader(shadow_tl_vert, sizeof shadow_tl_vert), sfs, ge_layout,
                                           VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, 9);
        if (vk.output_layer)
            point_tl_pipeline = make_pipeline(vk_shader(pshadow_tl_vert, sizeof pshadow_tl_vert), pfs, ge_layout,
                                              VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, 10);
    }
    VkSamplerCreateInfo shi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    shi.magFilter = shi.minFilter = VK_FILTER_LINEAR;
    shi.addressModeU = shi.addressModeV = shi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    shi.compareEnable = VK_TRUE;
    shi.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(vk.device, &shi, NULL, &shadow_sampler);
    for (int i = 0; i < 2; i++)
        shadow_extra[i] = vk_buffer(SHADOW_EXTRA_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1);
    extra = shadow_extra[0];
    shadow_img = vk_image(SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkDescriptorImageInfo sii = {shadow_sampler, shadow_img.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet sw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_SHADOW_MAP, 0, 1,
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
    VkWriteDescriptorSet pw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_POINT_SHADOWS, 0, 2,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pii, NULL, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &pw, 0, NULL);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, vk.pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    vkAllocateCommandBuffers(vk.device, &cai, shadow_cmds);
    shadow_cmd = shadow_cmds[0];
    vkAllocateCommandBuffers(vk.device, &cai, point_cmds);
    point_cmd = point_cmds[0];
    vk.before_submit = shadow_finish;
}

void shadow_settings(void) {
    point_shadows_on = !getenv("MHFU_POINT_SHADOWS") || atoi(getenv("MHFU_POINT_SHADOWS")) != 0;
}

/* vk_submit: the frame's buffers are free again */
void shadow_frame_reused(int frame) {
    extra = shadow_extra[frame];
    shadow_cmd = shadow_cmds[frame];
    vkResetCommandBuffer(shadow_cmd, 0);
    point_cmd = point_cmds[frame];
    vkResetCommandBuffer(point_cmd, 0);
}

/* The shadow map's pass, begun at the first thing in the world of a frame: cleared to the farthest */
static void shadow_begin(void) {
    if (shadow_recording)
        return;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(shadow_cmd, &bi);
    shadow_recording = 1;
    shadow_drawn = 1;
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
    vkCmdBindDescriptorSets(shadow_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_layout, 0, 1, &ge_set, 0, NULL);
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
    vkCmdBindDescriptorSets(point_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_layout, 0, 1, &ge_set, 0, NULL);
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
SAVED float rt_cull_margin;

void render_shadow_frame(const float *m, int on) {
    /* with shadows, what is just out of view is drawn too (MHFU_CULL_MARGIN: how far, 1200) */
    static float margin = -1;
    if (margin < 0)
        margin = getenv("MHFU_CULL_MARGIN") ? (float)atof(getenv("MHFU_CULL_MARGIN")) : 1200.0f;
    rt_cull_margin = on ? margin : 0.0f;
    if (on && memcmp(m, scene.sun_vp, sizeof scene.sun_vp)) {
        flush();                        /* what is batched was placed with the last */
        memcpy(scene.sun_vp, m, sizeof scene.sun_vp);
        /* the map's centre: the point at x = y = 0, depth 0.5 (the light's rows are orthonormal, scaled) */
        float sx = 1 / (m[0] * m[0] + m[4] * m[4] + m[8] * m[8]), sy = 1 / (m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
        float sz = 1 / (m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
        for (int i = 0; i < 3; i++)
            scene.focus[i] = -(m[4 * i] * m[12] * sx + m[4 * i + 1] * m[13] * sy + m[4 * i + 2] * (m[14] - 0.5f) * sz);
    }
    if (on != shadow_on)
        flush();
    shadow_on = on;
}

/* A batch of the world (first, n: its vertices) drawn again into the shadow maps, if it casts shadows: what is solid -
 * blended over what is there by its alpha too (much of the scenery is), where it is more than half opaque - with what
 * clipping cut off of it */
void shadow_draw(const DrawState *s, DrawPush pc, uint32_t first, uint32_t n) {
    int alpha_blend = s->blend && s->eq.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
                      s->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (shadow_on && (s->flags & DRAW_WORLD) && s->depth_write && (!s->blend || alpha_blend)) {
        if (alpha_blend)
            pc.flags |= DRAW_BLENDED;
        shadow_begin();
        vkCmdPushConstants(shadow_cmd, ge_layout, pc_stages, 0, sizeof pc, &pc);
        vkCmdDraw(shadow_cmd, n, 1, first, 0);
        if (extra_used > extra_batch) {
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(shadow_cmd, 0, 1, &extra.buffer, &zero);
            vkCmdDraw(shadow_cmd, (uint32_t)((extra_used - extra_batch) / sizeof(GpuVertex)), 1,
                      (uint32_t)(extra_batch / sizeof(GpuVertex)), 0);
            vkCmdBindVertexBuffers(shadow_cmd, 0, 1, &vertices.buffer, &zero);
        }
    } else if (shadow_on && (s->flags & DRAW_WORLD))
        shadow_begin();                 /* (it is sampled: made this frame, if empty) */
    if (point_shadows && shadow_on && (s->flags & DRAW_WORLD)) {
        point_begin();
        if (s->depth_write && (!s->blend || alpha_blend)) {
            if (alpha_blend)
                pc.flags |= DRAW_BLENDED;
            vkCmdPushConstants(point_cmd, ge_layout, pc_stages, 0, sizeof pc, &pc);
            uint32_t instances = 6u * (uint32_t)point_shadows;
            vkCmdDraw(point_cmd, n, instances, first, 0);
            if (extra_used > extra_batch) {
                VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(point_cmd, 0, 1, &extra.buffer, &zero);
                vkCmdDraw(point_cmd, (uint32_t)((extra_used - extra_batch) / sizeof(GpuVertex)), instances,
                          (uint32_t)(extra_batch / sizeof(GpuVertex)), 0);
                vkCmdBindVertexBuffers(point_cmd, 0, 1, &vertices.buffer, &zero);
            }
        }
    }
    extra_batch = extra_used;
}

/* A batch of the GPU's vertex stage's (MHFU_GPU_TL: its segments) drawn again into the shadow maps, as shadow_draw
 * would: its vertices placed in the world by the shaders (whole triangles - what the camera's near plane cuts off
 * casts shadows too) */
static void tl_segments(VkCommandBuffer cmd, VkPipeline pipeline, VkPipeline after, const TlSegment *seg, int nseg,
                        uint32_t instances, uint32_t per_record) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 1, 1, &tl_vertices.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, tl_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (int i = 0; i < nseg; i++)
        vkCmdDrawIndexed(cmd, seg[i].count, instances, seg[i].first, 0, seg[i].rec * per_record);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, after);    /* (the CPU's batches' again) */
}

void shadow_draw_tl(const DrawState *s, DrawPush pc, const TlSegment *seg, int nseg) {
    int alpha_blend = s->blend && s->eq.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
                      s->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    int casts = s->depth_write && (!s->blend || alpha_blend);
    if (alpha_blend)
        pc.flags |= DRAW_BLENDED;
    if (!shadow_on || !(s->flags & DRAW_WORLD))
        return;
    shadow_begin();                     /* (it is sampled: made this frame, if empty) */
    if (casts && shadow_tl_pipeline) {
        vkCmdPushConstants(shadow_cmd, ge_layout, pc_stages, 0, sizeof pc, &pc);
        tl_segments(shadow_cmd, shadow_tl_pipeline, shadow_pipeline, seg, nseg, 1, 1);
    }
    if (point_shadows) {
        point_begin();
        if (casts && point_tl_pipeline) {
            vkCmdPushConstants(point_cmd, ge_layout, pc_stages, 0, sizeof pc, &pc);
            tl_segments(point_cmd, point_tl_pipeline, point_pipeline, seg, nseg, 6u * (uint32_t)point_shadows, 12);
        }
    }
}

/* A batch not drawn: what clipping cut off of it goes too */
void shadow_skip(void) {
    extra_batch = extra_used;
}

/* A triangle of the current primitive (in the world) clipping cut (some of it is behind the camera): whole, for the
 * shadows */
void shadow_extra_triangle(const RVertex *a, const RVertex *b, const RVertex *c) {
    if (extra_used + 3 * sizeof(GpuVertex) > SHADOW_EXTRA_BYTES)
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

void shadow_end_frame(int next_point_shadows) {
    shadow_drawn = 0;
    extra_used = extra_batch = 0;
    point_shadows = next_point_shadows;
}
