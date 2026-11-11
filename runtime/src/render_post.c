/* Post-processing (post.c), at the end of the world (render_internal.h): once the world is drawn - at the first thing
 * drawn over it that is not in it (the HUD), or at the flip - what added light to it (fire, glows) is drawn again into
 * post.c's glow image, and the hunter's, monsters' and people's motion into its motion image, against the world's
 * depth; then post.c's passes, with the world's camera and the sun. */
#include "render_internal.h"

#include "post.h"

#include <string.h>

#include "present.vert.inc"

#define MAX_GLOW_DRAWS 8192
#define MAX_GLOW_SEGMENTS 16384
static VkPipeline glow_pipeline, glow_tess_pipeline, glow_tl_pipeline;
static Target *post_target, *post_done;     /* the world is being drawn into it; it has been post-processed */
static Target *post_held;                   /* the first thing over its world not in it is batched, not yet drawn */
static float post_view[16], post_world_view[12];    /* the world's camera (its last solid draw's) */
/* A draw adding light to the world, kept to be drawn again into the glow (kind 0); and the hunter's, monsters' and
 * people's, into the motion blur's image - their motion (1 flat, 2 tessellated, 3 the GPU's vertex stage's: first,
 * count its segments in glow_segs) */
static struct { DrawState s; DrawPush pc; uint32_t first, count; int kind; } glow_draws[MAX_GLOW_DRAWS];
static int nglow;
static TlSegment glow_segs[MAX_GLOW_SEGMENTS];
static int nglow_segs;

/* post.c's passes: a full-screen triangle (blend: added to what is there) */
static VkPipeline post_pipeline(VkShaderModule fs, VkPipelineLayout pl, VkFormat color, int blend) {
    return make_pipeline(vk_shader(present_vert, sizeof present_vert), fs, pl, color, VK_FORMAT_UNDEFINED,
                         blend ? 5 : 0);
}

/* post.c, and the pipelines drawing the glow and motion (the GE's shaders vs, fs; tess: tessellated too, or NULL; tl_vs:
 * the GPU's vertex stage's, or none) */
void post_setup(VkShaderModule vs, VkShaderModule fs, int tess, VkShaderModule tl_vs) {
    if (!post_init(render_scale, render_wide, shadow_img.view, shadow_sampler, post_pipeline))
        return;
    glow_pipeline = make_pipeline(vs, fs, ge_layout, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, 1);
    /* (the motion's too, whether or not it is wanted now: the settings may turn it on) */
    if (tl_vs)
        glow_tl_pipeline = make_pipeline(tl_vs, fs, ge_layout, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, 8);
    if (tess)
        glow_tess_pipeline = make_pipeline(vs, fs, ge_layout, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, 6);
}

/* gpu.c: the motion of what moves by itself is wanted (the motion blur's, the anti-aliasing's) */
int render_motion_wanted(void) {
    return glow_pipeline && post_motion();
}

static VkImg solid_depth;          /* (keep_solid_depth) */
static Target *solid_depth_of;

/* The world is drawn into t: what added light to it drawn again into the glow (against its depth), then post.c's
 * passes over it */
static void post_run(Target *t) {
    post_target = NULL;
    post_done = t;
    if (!glow_pipeline || !post_on())
        return;
    end_rendering();
    uint32_t fw = scaled_x(480), fh = scaled(272), px = scaled(RT_SIZE), pw = scaled_x(RT_SIZE);
    vk_transition(&t->depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    /* the glow (the draws adding light, again); then the motion blur's (the hunter's, monsters' and people's motion) */
    for (int pass = 0; pass < (post_motion() ? 2 : 1); pass++) {
        VkImg *g = pass ? post_motion_image() : post_glow();
        vk_transition(g, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g->view,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
        VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, t->depth.view,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {fw, fh}}, 1, 0, 1, &ca, &da, NULL};
        VkCommandBuffer cmd = vk_cmd();
        vkCmdBeginRendering(cmd, &ri);
        if (nglow) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ge_layout, 0, 1, &ge_set, 0, NULL);
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &zero);
            VkViewport vp = {0, 0, (float)pw, (float)px, 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
            VkBool32 blend = pass ? VK_FALSE : VK_TRUE;
            vk.SetColorBlendEnable(cmd, 0, 1, &blend);
            VkColorComponentFlags all = 0xF;
            vk.SetColorWriteMask(cmd, 0, 1, &all);
            VkPipeline bound = glow_pipeline;
            for (int i = 0; i < nglow; i++) {
                const DrawState *d = &glow_draws[i].s;
                int kind = glow_draws[i].kind;
                VkPipeline p = kind == 2 ? glow_tess_pipeline : kind == 3 ? glow_tl_pipeline : glow_pipeline;
                if (!p || (kind != 0) != pass)
                    continue;
                if (p != bound)
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
                VkRect2D sc = d->scissor;
                if (sc.offset.x >= (int32_t)fw || sc.offset.y >= (int32_t)fh)
                    continue;
                if (sc.offset.x + sc.extent.width > fw)
                    sc.extent.width = fw - (uint32_t)sc.offset.x;
                if (sc.offset.y + sc.extent.height > fh)
                    sc.extent.height = fh - (uint32_t)sc.offset.y;
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdSetDepthTestEnable(cmd, d->depth_test);
                VkCompareOp op = d->depth_op;   /* (the hunter's own depth: drawn again, it passes) */
                if (kind && op == VK_COMPARE_OP_GREATER)
                    op = VK_COMPARE_OP_GREATER_OR_EQUAL;
                if (kind && op == VK_COMPARE_OP_LESS)
                    op = VK_COMPARE_OP_LESS_OR_EQUAL;
                vkCmdSetDepthCompareOp(cmd, op);
                vk.SetColorBlendEquation(cmd, 0, 1, &d->eq);
                vkCmdSetBlendConstants(cmd, d->blend_const);
                vkCmdSetCullMode(cmd, d->cull);
                vkCmdSetFrontFace(cmd, d->front);
                vkCmdPushConstants(cmd, ge_layout, pc_stages, 0, sizeof glow_draws[i].pc, &glow_draws[i].pc);
                if (kind == 3) {
                    vkCmdBindVertexBuffers(cmd, 1, 1, &tl_vertices.buffer, &zero);
                    vkCmdBindIndexBuffer(cmd, tl_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                    for (uint32_t k = glow_draws[i].first; k < glow_draws[i].first + glow_draws[i].count; k++)
                        vkCmdDrawIndexed(cmd, glow_segs[k].count, 1, glow_segs[k].first, 0, glow_segs[k].rec);
                    continue;
                }
                vkCmdDraw(cmd, glow_draws[i].count, 1, glow_draws[i].first, 0);
            }
        }
        vkCmdEndRendering(cmd);
    }
    nglow = nglow_segs = 0;
    ge_lost();
    /* the camera, the sun, the shadow map */
    PostFrame f;
    memset(&f, 0, sizeof f);
    float m4[16] = {0}, inv[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 3; i++)
            m4[4 * c + i] = post_world_view[3 * c + i];
    m4[15] = 1;
    if (invert4(post_view, f.to_view) && invert4(m4, inv)) {
        f.shadows = shadow_on && shadow_drawn;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) {
                float v = 0;
                for (int k = 0; k < 4; k++)
                    v += scene.sun_vp[4 * k + r] * m4[4 * c + k];
                f.to_shadow[4 * c + r] = v;
            }
        float sw[3] = {-scene.sun_vp[2], -scene.sun_vp[6], -scene.sun_vp[10]}, len = 0;
        for (int k = 0; k < 3; k++) {
            f.sun[k] = inv[k] * sw[0] + inv[4 + k] * sw[1] + inv[8 + k] * sw[2];
            len += f.sun[k] * f.sun[k];
        }
        len = sqrtf(len);
        for (int k = 0; k < 3; k++)
            f.sun[k] = len > 0 ? f.sun[k] / len : 0;
        if (!(len > 0))
            f.shadows = 0;
        /* the focus: the point the camera looks at (the shadow map's centre) */
        if (shadow_on) {
            float c[3], fl = 0;
            for (int k = 0; k < 3; k++) {
                c[k] = inv[k] * scene.focus[0] + inv[4 + k] * scene.focus[1] + inv[8 + k] * scene.focus[2] + inv[12 + k];
                fl += c[k] * c[k];
            }
            f.focus = sqrtf(fl);
        }
    }
    /* the motion since the last frame: this frame's camera space to the world, to the last's camera, its screen */
    static float prev_view[16], prev_world_inv[16];
    static int prev_ok;
    if (invert4(m4, inv)) {
        if (prev_ok) {
            float a[16];
            mul4(prev_world_inv, m4, a);
            mul4(prev_view, a, f.to_prev);
            f.moved = 1;
        }
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                f.to_camera[4 * r + c] = inv[4 * c + r];
        memcpy(prev_view, post_view, sizeof prev_view);
        memcpy(prev_world_inv, inv, sizeof prev_world_inv);
        prev_ok = 1;
    } else
        prev_ok = 0;
    float top = fmaxf(scene.sun_rgb[0], fmaxf(scene.sun_rgb[1], scene.sun_rgb[2]));
    for (int k = 0; k < 3; k++)                 /* its hue, half way to white */
        f.sun_color[k] = 0.5f + 0.5f * (top > 0 ? scene.sun_rgb[k] / top : 1.0f);
    f.fog_color = scene.fog_color;
    f.jitter[0] = render_jitter[0];
    f.jitter[1] = render_jitter[1];
    post_apply(&t->color, solid_depth_of == t ? &solid_depth : &t->depth, &f);
}

/* The post-processing of t, then what is batched (the draw held) drawn over it */
static void post_run_held(Target *t) {
    end_rendering_kept();
    post_run(t);
    begin_rendering(t);
    flush();
}

/* A primitive drawn into t, with these flags (solid: of the solid world; same: batched with the last): once the world
 * is drawn, at the first thing over it not in it, the post-processing (once, until cleared). That first thing is held
 * in its batch until what follows it shows whether the world was done: the game draws some over the world part way
 * through (the village's haze over the sky, every other frame: #37), and the world drawn after it was left out */
/* The solid world's depth, for the effects (the motion blur, the anti-aliasing, haze, depth of field): t's copied as
 * the first thing in the world not solid writes its own over it - the forest's mist, close to the camera all over the
 * picture, was taken for the world there, and the camera's going round the hunter smeared all of it (#72) */
static void keep_solid_depth(Target *t) {
    if (solid_depth.width != t->depth.width || solid_depth.height != t->depth.height) {
        if (solid_depth.image) {
            vk_finish();
            vk_image_free(&solid_depth);
        }
        solid_depth = vk_image(t->depth.width, t->depth.height, VK_FORMAT_D32_SFLOAT,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    flush();
    Target *was = rendering_target();
    end_rendering();
    vk_transition(&t->depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_transition(&solid_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy = {{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0},
                        {t->depth.width, t->depth.height, 1}};
    vkCmdCopyImage(vk_cmd(), t->depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, solid_depth.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    solid_depth_of = t;
    if (was)
        begin_rendering(was);
}

void post_state(Target *t, uint32_t flags, int solid, int depth_write, int clear, int same) {
    if (clear && t == solid_depth_of)
        solid_depth_of = NULL;
    if (t == post_target && (flags & DRAW_WORLD) && !solid && depth_write && !clear && solid_depth_of != t)
        keep_solid_depth(t);
    if (post_held && !(t == post_held && !clear && same)) {
        Target *h = post_held;
        post_held = NULL;
        if (t == h && (flags & DRAW_WORLD) && !clear)
            flush();                    /* the world goes on: it was in it */
        else
            post_run_held(h);
    }
    if (post_target && t == post_target && !(flags & DRAW_WORLD) && !clear && !post_held) {
        flush();
        post_held = t;
    }
    if (clear && t == post_done)
        post_done = NULL;
    if (glow_pipeline && post_on() && (flags & DRAW_WORLD) && t != post_done) {
        post_target = t;
        if (solid) {
            memcpy(post_view, prim_view, sizeof post_view);
            memcpy(post_world_view, prim_world_view, sizeof post_world_view);
        }
    }
}

/* A batch drawn into t (first, n: its vertices; tessellated: drawn so too): kept if it adds light to the world (fire,
 * glows), or moves in it (the hunter, monsters, people: their motion) */
void post_keep(Target *t, const DrawState *s, const DrawPush *pc, uint32_t first, uint32_t n, int tessellated) {
    if (!glow_pipeline || !post_on() || t != post_target)
        return;
    if ((s->flags & DRAW_WORLD) && s->blend && s->eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE &&
        s->eq.colorBlendOp == VK_BLEND_OP_ADD && nglow < MAX_GLOW_DRAWS) {
        glow_draws[nglow].s = *s;
        glow_draws[nglow].pc = *pc;
        glow_draws[nglow].first = first;
        glow_draws[nglow].kind = 0;
        glow_draws[nglow++].count = n;
    }
    if (post_motion() && (s->flags & DRAW_KIND) > DRAW_WORLD && !s->blend && s->depth_write) {
        for (int kind = 1; kind <= (tessellated ? 2 : 1) && nglow < MAX_GLOW_DRAWS; kind++) {
            glow_draws[nglow].s = *s;
            glow_draws[nglow].pc = *pc;
            glow_draws[nglow].pc.flags |= DRAW_MOTION;
            glow_draws[nglow].first = first;
            glow_draws[nglow].kind = kind;
            glow_draws[nglow++].count = n;
        }
    }
}

/* A batch of the GPU's vertex stage's (its segments) drawn into t: kept as post_keep keeps the motion of the hunter,
 * monsters and people (what adds light is the CPU's vertex stage's: gpu.c) */
void post_keep_tl(Target *t, const DrawState *s, const DrawPush *pc, const TlSegment *seg, int nseg) {
    if (!glow_tl_pipeline || t != post_target || !post_motion() || (s->flags & DRAW_KIND) <= DRAW_WORLD || s->blend ||
        !s->depth_write || nglow == MAX_GLOW_DRAWS || nglow_segs + nseg > MAX_GLOW_SEGMENTS)
        return;
    memcpy(&glow_segs[nglow_segs], seg, sizeof *seg * (size_t)nseg);
    glow_draws[nglow].s = *s;
    glow_draws[nglow].pc = *pc;
    glow_draws[nglow].pc.flags |= DRAW_MOTION;
    glow_draws[nglow].first = (uint32_t)nglow_segs;
    glow_draws[nglow].kind = 3;
    glow_draws[nglow++].count = (uint32_t)nseg;
    nglow_segs += nseg;
}

/* The frame shown or read: the post-processing still to do */
void post_finish(void) {
    if (post_held) {
        Target *h = post_held;
        post_held = NULL;
        post_run_held(h);
    } else if (post_target)
        post_run(post_target);
}

void post_frame_done(void) {
    post_target = post_done = post_held = NULL;
    solid_depth_of = NULL;
    nglow = nglow_segs = 0;
    post_end_frame();
}
