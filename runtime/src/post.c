/* Post-processing of the world's picture: run by the renderer (render.c) once the world is drawn, before the HUD.
 *
 * - Motion blur: each pixel smeared along the way it moved on the screen since the last frame (the camera's motion:
 *   from the depth, through the last frame's camera), as long as the shutter of a camera filming at the game's 30
 *   frames a second stays open (MHFU_MOTION_BLUR: the share of the 30th of a second, 0 none); the hunter, monsters and
 *   people left sharp.
 * - Bloom: the light the effects add (fire, glows - render.c draws those again into an image of their own, the glow)
 *   and the brightest of the picture's own, spread by a chain of ever smaller images (each half the last) and added
 *   back up (MHFU_BLOOM: its strength, 0 none).
 * - Light shafts: the sunlight the air scatters towards the camera, marched through the sun's shadow map at half
 *   resolution - shafts through gaps, a glow of haze towards the sun (MHFU_SHAFTS: their strength, 0 none).
 * - Depth of field: what is beyond the point the camera looks at (the hunter) blurred, the more the farther (a disc at
 *   half resolution; MHFU_DOF: how much, 0 none - the default).
 * - Sharpening, contrast adaptive (MHFU_SHARPEN: 0 none .. 1).
 * - Colour grading: saturation, contrast, warmth, vignette (MHFU_SATURATION, MHFU_CONTRAST, MHFU_WARMTH,
 *   MHFU_VIGNETTE).
 *
 * The picture is copied out, the passes made from the copy, and the last pass writes the result back. MHFU_POST=0:
 * none of it. */
#include "post.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bloom_down.frag.inc"
#include "bloom_up.frag.inc"
#include "shafts.frag.inc"
#include "shafts_blur.frag.inc"
#include "post.frag.inc"
#include "dof_prefilter.frag.inc"
#include "dof_gather.frag.inc"
#include "motion.frag.inc"

#define LEVELS 10               /* the bloom's images, at most: half the picture, a quarter... */
#define SPREAD 4                /* below the one as wide as the PSP's picture, so many (the same glow at any scale) */
#define PER_FRAME 8             /* times a frame (the frame dumps draw it again) */
#define PASSES (2 * LEVELS + 7)
#define INPUTS 6

typedef struct {
    float to_view[16], to_shadow[16], texel[4], area[4], sun[4], sun_color[4], bloom[4], grade[4], dof[4];
} Push;

static int on = -1;
static float bloom = 3.0f, bloom_threshold = 0.9f, bloom_picture = 0.35f, bloom_glow = 0.6f;
static float shafts = 0.0f, shafts_contrast = 2.0f;
static float saturation = 1.08f, contrast = 1.05f, warmth = 0.0f, vignette = 0.2f;
static float dof = 0.0f, sharpen = 0.4f, motion = 0.5f;
static int fps;
static uint32_t fw, fh;
static int first, nlevels;      /* the bloom's: the level as wide as the PSP's picture (it is made up to), how many */
static VkImg scene, moved, glow, levels[LEVELS], shaft_img[2], dof_img[2], dummy, dummy_depth;
static VkPipeline p_motion, p_down, p_up, p_shafts, p_blur, p_final, p_dof_prefilter, p_dof_gather;
static VkPipelineLayout layout;
static VkDescriptorSetLayout set_layout;
static VkDescriptorPool pool;
static VkDescriptorSet sets[2][PER_FRAME][PASSES];
static VkSampler linear, nearest, shadow_sampler;
static VkImageView shadow_view;
static int used;

static float env(const char *name, float def) {
    const char *s = getenv(name);
    return s ? (float)atof(s) : def;
}

int post_on(void) {
    return on > 0;
}

int post_motion(void) {
    return on > 0 && motion > 0;
}

VkImg *post_glow(void) {
    return &glow;
}

int post_init(int scale, VkImageView shadow, VkSampler shadow_s, PostPipeline make) {
    on = !getenv("MHFU_POST") || atoi(getenv("MHFU_POST")) != 0;
    bloom = env("MHFU_BLOOM", bloom);
    shafts = env("MHFU_SHAFTS", shafts);
    saturation = env("MHFU_SATURATION", saturation);
    contrast = env("MHFU_CONTRAST", contrast);
    warmth = env("MHFU_WARMTH", warmth);
    vignette = env("MHFU_VIGNETTE", vignette);
    dof = env("MHFU_DOF", dof);
    sharpen = env("MHFU_SHARPEN", sharpen);
    motion = env("MHFU_MOTION_BLUR", motion);
    fps = (int)env("MHFU_FPS", 0);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.gpu, &props);
    if (props.limits.maxPushConstantsSize < sizeof(Push))
        on = 0;
    if (!on)
        return 0;
    shadow_view = shadow;
    shadow_sampler = shadow_s;
    fw = 480u * (uint32_t)scale;
    fh = 272u * (uint32_t)scale;
    VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    scene = vk_image(fw, fh, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    moved = vk_image(fw, fh, VK_FORMAT_R8G8B8A8_UNORM, rt, VK_IMAGE_ASPECT_COLOR_BIT);
    glow = vk_image(fw, fh, VK_FORMAT_R16G16B16A16_SFLOAT, rt, VK_IMAGE_ASPECT_COLOR_BIT);
    first = 0;                          /* (the level nearest 480 wide) */
    while (first + 1 < LEVELS - SPREAD && fabsf(log2f((float)(fw >> (first + 2)) / 480.0f)) <
                                              fabsf(log2f((float)(fw >> (first + 1)) / 480.0f)))
        first++;
    nlevels = first + SPREAD + 1;
    for (int i = 0; i < nlevels; i++)
        levels[i] = vk_image(fw >> (i + 1), fh >> (i + 1), VK_FORMAT_R16G16B16A16_SFLOAT, rt, VK_IMAGE_ASPECT_COLOR_BIT);
    for (int i = 0; i < 2; i++) {
        shaft_img[i] = vk_image(fw / 2, fh / 2, VK_FORMAT_R16G16B16A16_SFLOAT, rt, VK_IMAGE_ASPECT_COLOR_BIT);
        dof_img[i] = vk_image(fw / 2, fh / 2, VK_FORMAT_R16G16B16A16_SFLOAT, rt, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    dummy = vk_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    dummy_depth = vk_image(1, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
    vk_transition(&dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_transition(&dummy_depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    VkDescriptorSetLayoutBinding b[INPUTS];
    for (uint32_t i = 0; i < INPUTS; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                              VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, INPUTS, b};
    vkCreateDescriptorSetLayout(vk.device, &dl, NULL, &set_layout);
    enum { SETS = 2 * PER_FRAME * PASSES };
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, INPUTS * SETS};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SETS, 1, &ps};
    vkCreateDescriptorPool(vk.device, &dpi, NULL, &pool);
    VkDescriptorSetLayout layouts[SETS];
    for (int i = 0; i < SETS; i++)
        layouts[i] = set_layout;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, SETS, layouts};
    vkAllocateDescriptorSets(vk.device, &dai, &sets[0][0][0]);
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout, 1, &pr};
    vkCreatePipelineLayout(vk.device, &pli, NULL, &layout);
    VkFormat hdr = VK_FORMAT_R16G16B16A16_SFLOAT;
    p_motion = make(vk_shader(motion_frag, sizeof motion_frag), layout, VK_FORMAT_R8G8B8A8_UNORM, 0);
    p_down = make(vk_shader(bloom_down_frag, sizeof bloom_down_frag), layout, hdr, 0);
    p_up = make(vk_shader(bloom_up_frag, sizeof bloom_up_frag), layout, hdr, 1);
    p_shafts = make(vk_shader(shafts_frag, sizeof shafts_frag), layout, hdr, 0);
    p_blur = make(vk_shader(shafts_blur_frag, sizeof shafts_blur_frag), layout, hdr, 0);
    p_final = make(vk_shader(post_frag, sizeof post_frag), layout, VK_FORMAT_R8G8B8A8_UNORM, 0);
    p_dof_prefilter = make(vk_shader(dof_prefilter_frag, sizeof dof_prefilter_frag), layout, hdr, 0);
    p_dof_gather = make(vk_shader(dof_gather_frag, sizeof dof_gather_frag), layout, hdr, 0);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk.device, &sci, NULL, &linear);
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(vk.device, &sci, NULL, &nearest);
    return 1;
}

void post_end_frame(void) {
    used = 0;
}

/* A pass's inputs: four images (NULL: none), the depth, the shadow map (if it is used) */
static void set_inputs(VkDescriptorSet ds, VkImg *a, VkImg *b, VkImg *c, VkImg *d, VkImg *depth, int shadow) {
    VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dro = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo ii[INPUTS] = {
        {linear, (a ? a : &dummy)->view, ro},
        {linear, (b ? b : &dummy)->view, ro},
        {nearest, (depth ? depth : &dummy_depth)->view, dro},
        {shadow_sampler, shadow ? shadow_view : dummy_depth.view, dro},
        {linear, (c ? c : &dummy)->view, ro},
        {linear, (d ? d : &dummy)->view, ro},
    };
    VkWriteDescriptorSet w[INPUTS];
    for (uint32_t i = 0; i < INPUTS; i++)
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, i, 0, 1,
                                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[i], NULL, NULL};
    vkUpdateDescriptorSets(vk.device, INPUTS, w, 0, NULL);
}

/* A pass: a triangle over w x h of an image (load: blended over what is there) */
static void draw(VkImg *dst, uint32_t w, uint32_t h, VkPipeline p, VkDescriptorSet ds, const Push *pc, int load) {
    vk_transition(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkCommandBuffer cmd = vk_cmd();
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, dst->view,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
        load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, 1, 0, 1, &ca, NULL, NULL};
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *pc, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void post_apply(VkImg *color, VkImg *depth, const PostFrame *f) {
    if (on <= 0 || used >= PER_FRAME)
        return;
    VkDescriptorSet *s = sets[vk.frame][used++];
    int k = 0;
    VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    /* the picture, copied out */
    vk_transition(color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_transition(&scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                        {fw, fh, 1}};
    vkCmdCopyImage(vk_cmd(), color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_transition(&scene, ro);
    vk_transition(&glow, ro);
    vk_transition(depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    Push pc;
    memset(&pc, 0, sizeof pc);
    memcpy(pc.to_view, f->to_view, sizeof pc.to_view);
    memcpy(pc.to_shadow, f->to_shadow, sizeof pc.to_shadow);
    memcpy(pc.area, (float[4]){(float)fw, (float)fh, (float)fw / 480.0f, env("MHFU_POST_DEBUG", 0)}, sizeof pc.area);
    int shafts_on = shafts > 0 && f->shadows;
    memcpy(pc.sun, (float[4]){f->sun[0], f->sun[1], f->sun[2], shafts_on ? shafts * 3e-5f : 0}, sizeof pc.sun);
    memcpy(pc.sun_color, (float[4]){f->sun_color[0], f->sun_color[1], f->sun_color[2], shafts_contrast},
           sizeof pc.sun_color);
    memcpy(pc.bloom, (float[4]){bloom / (SPREAD + 1), bloom_threshold, bloom_picture, bloom_glow}, sizeof pc.bloom);
    memcpy(pc.grade, (float[4]){saturation, contrast, warmth, vignette}, sizeof pc.grade);
    int dof_on = dof > 0 && f->focus > 0;
    /* (the blur's largest: a dozen PSP pixels across at full strength) */
    memcpy(pc.dof, (float[4]){f->focus, 0.35f, dof_on ? dof * 3.0f * (float)fw / 480.0f : 0, sharpen},
           sizeof pc.dof);

    /* motion blur: the picture smeared, the rest made from it. Its length: the motion of a frame shown, times the
     * frames shown in the shutter's time (1 without frames of its own between the game's) */
    VkImg *picture = &scene;
    if (motion > 0 && f->moved) {
        Push mp = pc;
        memcpy(mp.to_shadow, f->to_prev, sizeof mp.to_shadow);
        float frames = motion * (fps > 30 ? (float)fps / 30.0f : 1.0f);
        memcpy(mp.texel, (float[4]){frames, 24.0f * (float)fw / 480.0f, 0, 0}, sizeof mp.texel);
        set_inputs(s[k], &scene, &glow, NULL, NULL, depth, 0);
        draw(&moved, fw, fh, p_motion, s[k++], &mp, 0);
        vk_transition(&moved, ro);
        picture = &moved;
    }
    /* bloom: down the levels, then back up, each adding the smaller */
    if (bloom > 0) {
        for (int i = 0; i < nlevels; i++) {
            VkImg *src = i ? &levels[i - 1] : picture;
            set_inputs(s[k], src, &glow, NULL, NULL, NULL, 0);
            memcpy(pc.texel, (float[4]){1.0f / (float)src->width, 1.0f / (float)src->height, i == 0, 0},
                   sizeof pc.texel);
            draw(&levels[i], levels[i].width, levels[i].height, p_down, s[k++], &pc, 0);
            vk_transition(&levels[i], ro);
        }
        for (int i = nlevels - 2; i >= first; i--) {    /* (the tighter left out: they whiten the flames) */
            VkImg *src = &levels[i + 1];
            set_inputs(s[k], src, NULL, NULL, NULL, NULL, 0);
            memcpy(pc.texel, (float[4]){1.0f / (float)src->width, 1.0f / (float)src->height, 0, 0}, sizeof pc.texel);
            draw(&levels[i], levels[i].width, levels[i].height, p_up, s[k++], &pc, 1);
            vk_transition(&levels[i], ro);
        }
    }
    /* light shafts: at half resolution, blurred across and down */
    if (shafts_on) {
        uint32_t hw = fw / 2, hh = fh / 2;
        set_inputs(s[k], NULL, NULL, NULL, NULL, depth, 1);
        draw(&shaft_img[0], hw, hh, p_shafts, s[k++], &pc, 0);
        for (int pass = 0; pass < 2; pass++) {
            vk_transition(&shaft_img[pass], ro);
            set_inputs(s[k], &shaft_img[pass], NULL, NULL, NULL, NULL, 0);
            memcpy(pc.texel, (float[4]){pass == 0, pass == 1, 0, 0}, sizeof pc.texel);
            draw(&shaft_img[1 - pass], hw, hh, p_blur, s[k++], &pc, 0);
        }
        vk_transition(&shaft_img[0], ro);
    }
    /* depth of field: the picture at half resolution with how blurred each pixel is, then blurred */
    if (dof_on) {
        set_inputs(s[k], picture, NULL, NULL, NULL, depth, 0);
        draw(&dof_img[0], fw / 2, fh / 2, p_dof_prefilter, s[k++], &pc, 0);
        vk_transition(&dof_img[0], ro);
        set_inputs(s[k], &dof_img[0], NULL, NULL, NULL, NULL, 0);
        draw(&dof_img[1], fw / 2, fh / 2, p_dof_gather, s[k++], &pc, 0);
        vk_transition(&dof_img[1], ro);
    }
    /* the picture, back */
    set_inputs(s[k], picture, bloom > 0 ? &levels[first] : NULL, shafts_on ? &shaft_img[0] : NULL,
               dof_on ? &dof_img[1] : NULL, depth, 0);
    draw(color, fw, fh, p_final, s[k++], &pc, 0);
}
