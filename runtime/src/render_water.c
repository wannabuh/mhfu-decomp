/* Water: the game's lakes and rivers are a texture scrolled over a flat surface, blended over what is under it. Those
 * draws are found (blended by their alpha, in the world, their texture's coordinates moving from frame to frame, not
 * the colour of sand) and
 * drawn as water instead (ge.frag): what is under it seen through it, bent by ripples and tinted by the water's colour
 * the deeper it is, what is above it reflected (found in the picture, screen space), the sky where nothing is, and the
 * sun glinting - as much reflected as the angle makes it (Fresnel). ge.frag also checks the surface faces up and is
 * below the eye; anything else found stays as the game draws it.
 *
 * What is under and above it is the picture as it is at the first water drawn into it (the solid world, its occlusion
 * and the grass): copied then, colour and depth, for the water's draws to read. MHFU_WATER=0: as the game draws it. */
#include "settings.h"
#include "render_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int water_on;
static VkImg copy_color, copy_depth;
static uint32_t copy_px;
static unsigned long copied_frame;
static Target *copied;
static VkSampler linear_clamp, nearest_clamp;
/* the texture coordinates' offset each texture was last drawn with, and the frame it last moved */
static uint32_t offset_seen[MAX_TEXTURES][2];
static unsigned long moved_at[MAX_TEXTURES];
#define STILL_FRAMES 240            /* frames shown since its offset moved for a texture to stop being water */

void water_settings(void) {
    water_on = !getenv("MHFU_WATER") || atoi(getenv("MHFU_WATER")) != 0;
}

void water_init(void) {
    water_settings();
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk.device, &sci, NULL, &linear_clamp);
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(vk.device, &sci, NULL, &nearest_clamp);
}

/* The copies, the size of a render target (made again when that changes) */
static void copies_sized(void) {
    uint32_t px = scaled(RT_SIZE);
    if (copy_px == px && copy_color.image)
        return;
    retire(&copy_color);
    retire(&copy_depth);
    copy_color = vk_image(px, px, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT);
    copy_depth = vk_image(px, px, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
    copy_px = px;
    VkDescriptorImageInfo ci = {linear_clamp, copy_color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo di = {nearest_clamp, copy_depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_SCENE_COLOR, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ci, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_SCENE_DEPTH, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &di, NULL, NULL},
    };
    vkUpdateDescriptorSets(vk.device, 2, w, 0, NULL);
}

/* The picture as it is now (colour and depth) into the copies, and drawing into it carried on */
static void copy_picture(Target *t) {
    end_rendering();
    copies_sized();
    VkCommandBuffer cmd = vk_cmd();
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                     {copy_px, copy_px, 1}};
    vk_transition(&t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_transition(&copy_color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cmd, t->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_color.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vk_transition(&t->depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_transition(&copy_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(cmd, t->depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_depth.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vk_transition(&copy_color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_transition(&copy_depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    begin_rendering(t);
    ge_lost();
}

/* render.c: is the primitive about to be batched water? (Its picture copied, if it is the first water drawn into it
 * this frame.) */
int water_prim(const uint32_t *reg, const DrawState *s) {
    if (!water_on || !(s->flags & DRAW_WORLD) || !s->blend || (reg[0xDF] & 0xFFF) != 0x32 || s->tex_slot >= MAX_TEXTURES ||
        (s->flags & DRAW_LIT_PIXEL))
        return 0;
    uint32_t slot = s->tex_slot, u = reg[0x4A] & 0xFFFFFF, v = reg[0x4B] & 0xFFFFFF;
    /* (not scrolled itself: another draw scrolling the same texture does not make it water - the jungle's roots and
     * rocks share theirs with something that does, #32 - and it takes no part in the texture's tracking) */
    if (!u && !v)
        return 0;
    if (offset_seen[slot][0] != u || offset_seen[slot][1] != v) {
        if (moved_at[slot] || offset_seen[slot][0] || offset_seen[slot][1])
            moved_at[slot] = frame_no;
        offset_seen[slot][0] = u;
        offset_seen[slot][1] = v;
    }
    if (!moved_at[slot] || frame_no - moved_at[slot] > STILL_FRAMES)
        return 0;
    /* (and gpu.c's water_draw has not said otherwise: a dark layer drifting over a hillside, a cave's mist - #33, #41) */
    if (reg[0xFE] & 1)
        return 0;
    /* (sand flowing - the desert's sand sea, scrolled and blended as water is - told by its colour: red over green
     * over blue, as sand and earth are; water is blue, green or grey) */
    const float *c = tex_average[slot];
    if (c[0] > c[2] * 1.25f && c[1] > c[2] * 1.05f)
        return 0;
    if (copied_frame != frame_no || copied != s->target) {
        flush();
        copy_picture(s->target);
        copied_frame = frame_no;
        copied = s->target;
    }
    return 1;
}

/* The depth under the water in t, as it was before the first water drawn into it this frame (none: not copied) */
VkImageView water_under(Target *t) {
    return copied_frame == frame_no && copied == t ? copy_depth.view : VK_NULL_HANDLE;
}

/* The water's two light sets (ge.frag): the world to the clip space (dir) and back (color); then the eye (w: 1 if
 * nearer is a greater depth), the sun's colour, the time (seconds), the visible part of the picture (x, y of 0..1) */
uint32_t water_light_set(void) {
    static uint32_t set = ~0u;
    static unsigned long set_frame;
    static float set_clip[16];
    float clip[16], inv[16];
    if (!world_to_clip(prim_view, prim_world_view, clip) || !invert4(clip, inv))
        return ~0u;
    if (set != ~0u && set_frame == frame_no && !memcmp(clip, set_clip, sizeof clip))
        return set;
    if (light_sets_used >= LIGHT_SETS - 4)
        return ~0u;
    set = (uint32_t)vk.frame * LIGHT_SETS + light_sets_used;
    light_sets_used += 2;
    set_frame = frame_no;
    memcpy(set_clip, clip, sizeof clip);
    LightSet l[2];
    memset(l, 0, sizeof l);
    memcpy(l[0].dir, clip, sizeof clip);
    memcpy(l[0].color, inv, sizeof inv);
    int greater = scene.depth_op == VK_COMPARE_OP_GREATER || scene.depth_op == VK_COMPARE_OP_GREATER_OR_EQUAL;
    memcpy(l[1].dir[0], (float[4]){prim_world_view[9], prim_world_view[10], prim_world_view[11], greater ? 1.0f : 0.0f},
           16);
    memcpy(l[1].dir[1], (float[4]){scene.sun_rgb[0], scene.sun_rgb[1], scene.sun_rgb[2], 0}, 16);
    memcpy(l[1].dir[2], (float[4]){(float)scene_time(), 480.0f / RT_SIZE, 272.0f / RT_SIZE, 0}, 16);
    memcpy((LightSet *)light_buf.map + set, l, sizeof l);
    return set;
}
