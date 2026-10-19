/* The ground grass's drawing (render_internal.h; grass.c makes the blades from the scenery's triangles given it here):
 * once the solid world is drawn and its occlusion applied (render_ao.c), the tiles whose blades are ready uploaded into
 * their slots of the pool (near slots and far ones, grass.h), then those in view drawn - each blade an instance (grass.vert)
 * - with the world's camera, its depth test, the sun's shadow map, the trodden grass, the occlusion, the fog
 * (MHFU_GRASS=0: none; MHFU_GRASS_DENSITY, MHFU_GRASS_FULL: full density this near, MHFU_GRASS_DISTANCE: the reach). Only with the sun's shadows (the scenery's sun shading). */
#include "render_internal.h"

#include "grass.h"

#include <stdlib.h>
#include <string.h>

#include "grass.vert.inc"
#include "grass.frag.inc"

int grass_on;
static VkPipeline grass_pipeline;
static VkBuf grass_pool, trail_buf;
static unsigned long grass_frame_no;    /* the frame the grass was last told of (grass_frame) */

/* The occlusion of the ground its blades grow from (render_ao.c's; again when its image is re-made) */
void grass_bind_ao(void) {
    if (!ao_image())
        return;
    VkDescriptorImageInfo gi = {point_sampler, ao_image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet gw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_AO, 0, 1,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &gi, NULL, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &gw, 0, NULL);
}

void grass_init(void) {
    grass_on = getenv("MHFU_GRASS") ? atof(getenv("MHFU_GRASS")) > 0 : 1;
    /* the trodden grass's window, a copy each frame in flight */
    trail_buf = vk_buffer(2 * GRASS_TRAIL * GRASS_TRAIL * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
    memset(trail_buf.map, 0, 2 * GRASS_TRAIL * GRASS_TRAIL * sizeof(uint32_t));
    VkDescriptorBufferInfo tbi = {trail_buf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet tw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_TRAIL, 0, 1,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &tbi, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &tw, 0, NULL);
    grass_bind_ao();
    if (!grass_on)
        return;
    /* (MHFU_GRASS, other than 1, is the density too, as before MHFU_GRASS_DENSITY) */
    const char *dens = getenv("MHFU_GRASS_DENSITY") ? getenv("MHFU_GRASS_DENSITY") : getenv("MHFU_GRASS");
    grass_settings(dens ? (float)atof(dens) : 1.0f,
                   getenv("MHFU_GRASS_DISTANCE") ? (float)atof(getenv("MHFU_GRASS_DISTANCE")) : 3000.0f,
                   getenv("MHFU_GRASS_FULL") ? (float)atof(getenv("MHFU_GRASS_FULL")) : 700.0f);
    grass_pipeline = make_pipeline(vk_shader(grass_vert, sizeof grass_vert), vk_shader(grass_frag, sizeof grass_frag),
                                   ge_layout, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 7);
    grass_pool = vk_buffer((VkDeviceSize)grass_pool_blades() * sizeof(GrassBlade),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
}

double rt_game_30ths(void);         /* thread.c: the game's time in 30ths of a second */

/* Whether this frame lays the grass's ground (the camera's place noted at its first ask). Once a 30th of a second (a
 * frame of the game's at its own rate): the frames shown between draw the same scenery again */
int grass_laying(void) {
    static double game_30th = -1;
    static int laying;
    if (grass_frame_no != frame_no) {
        grass_frame_no = frame_no;
        laying = floor(rt_game_30ths()) != game_30th;
        game_30th = floor(rt_game_30ths());
        if (laying)
            grass_frame(&prim_world_view[9], scene_time());
    }
    return laying;
}

/* The scenery's triangle, for the grass's ground (as the frame lays it) */
void grass_scenery(const DrawState *d, const RVertex *a, const RVertex *b, const RVertex *c) {
    if (!grass_laying())
        return;
    const RVertex *v[3] = {a, b, c};
    float p[3][3], uv[3][2], col[3][3];
    float tw = d->tex_w > 0 ? d->tex_w : 1, th = d->tex_h > 0 ? d->tex_h : 1;
    int replace = (d->flags & DRAW_TFUNC) == 3;
    for (int i = 0; i < 3; i++) {
        p[i][0] = v[i]->wx;
        p[i][1] = v[i]->wy;
        p[i][2] = v[i]->wz;
        uv[i][0] = v[i]->u / tw;
        uv[i][1] = v[i]->v / th;
        col[i][0] = replace ? 255 : v[i]->r;
        col[i][1] = replace ? 255 : v[i]->g;
        col[i][2] = replace ? 255 : v[i]->b;
    }
    uint32_t slot = d->tex_slot;
    int has = slot < MAX_TEXTURES && tex_small[slot];
    grass_ground(p, uv, col, has ? tex_small[slot] : NULL, has ? tex_small_w[slot] : 0, has ? tex_small_h[slot] : 0,
                 (d->flags & DRAW_DOUBLE) != 0);
}

/* The grass, into t (its solid world drawn, with its occlusion) */
void grass_draw(Target *t) {
    if (!grass_pipeline || !shadow_on)
        return;
    end_rendering();
    /* the tiles made ready: two a frame, or more of the far ones (an eighth of the blades each) */
    int uploaded = 0;
    size_t budget = 2 * (size_t)grass_slot_blades();
    for (int k = 0; k < 16 && budget; k++) {
        int slot, n;
        const GrassBlade *b;
        size_t size;
        if (staging_used + (size_t)grass_slot_blades() * sizeof(GrassBlade) > STAGING_BYTES ||
            !grass_build(&slot, &b, &n))
            break;
        size = (size_t)n * sizeof(GrassBlade);
        budget -= (size_t)n < budget ? (size_t)n : budget;
        if (!size)
            continue;
        memcpy((uint8_t *)staging.map + staging_used, b, size);
        VkBufferCopy copy = {staging_used, (VkDeviceSize)grass_slot_base(slot) * sizeof(GrassBlade), size};
        vkCmdCopyBuffer(vk_cmd(), staging.buffer, grass_pool.buffer, 1, &copy);
        staging_used += (size + 255) & ~(size_t)255;
        uploaded = 1;
    }
    if (uploaded) {
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                               VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
        vkCmdPipelineBarrier2(vk_cmd(), &di);
    }
    static GrassDraw *tiles;
    if (!tiles)
        tiles = malloc((size_t)grass_slots() * sizeof *tiles);
    int ntiles = grass_tiles(tiles, grass_slots());
    if (!ntiles || light_sets_used + 1 > LIGHT_SETS - 2)
        return;
    /* the world to the clip space, with the world's camera */
    float clip[16];
    if (!world_to_clip(scene.view, scene.world_view, clip))
        return;
    for (int c = 0; c < 4; c++) {           /* (the anti-aliasing's offset, as the GE's world has it) */
        clip[4 * c] += render_jitter[0] * 2.0f / (float)scaled(RT_SIZE) * clip[4 * c + 3];
        clip[4 * c + 1] += render_jitter[1] * 2.0f / (float)scaled(RT_SIZE) * clip[4 * c + 3];
    }
    LightSet cam;
    memset(&cam, 0, sizeof cam);
    memcpy(cam.dir, clip, sizeof clip);
    const float *eye = &scene.world_view[9];
    double now = scene_time();
    memcpy(cam.color[0], (float[4]){eye[0], eye[1], eye[2], 0}, 16);
    memcpy(cam.color[1], (float[4]){(float)fmod(now, 3600.0), sway > 0 ? sway : 0, (float)scaled(RT_SIZE) / 2.0f,
                                    grass_radius()}, 16);
    cam.color[0][3] = grass_full();
    memcpy(cam.color[2], (float[4]){scene.fog[0], scene.fog[1], scene.fog[2], 0}, 16);
    float fc[4];
    unpack_rgb(scene.fog_color, fc);
    memcpy(cam.color[3], fc, 16);
    /* the trodden grass: pressed by the feet (the last frame's, found whole) */
    static float fd[MAX_FEET * GRASS_FOOT];
    for (int i = 0; i < scene.nfeet; i++) {
        const Foot *f = &scene.feet[i];
        memcpy(fd + i * GRASS_FOOT, (float[GRASS_FOOT]){f->x, f->y, f->z, f->r, f->vx, f->vz}, sizeof(float) * GRASS_FOOT);
    }
    int ox, oz;
    uint32_t trail_at = (uint32_t)vk.frame * GRASS_TRAIL * GRASS_TRAIL;
    grass_trail(fd, scene.nfeet, now, (uint32_t *)trail_buf.map + trail_at, &ox, &oz);
    uint32_t cam_set = (uint32_t)vk.frame * LIGHT_SETS + light_sets_used++;
    memcpy((LightSet *)light_buf.map + cam_set, &cam, sizeof cam);
    GrassPush pc;
    memcpy(pc.light, scene.sun_vp, sizeof pc.light);
    pc.camera = cam_set;
    pc.trail = trail_at;
    pc.trail_x = ox;
    pc.trail_z = oz;
    pc.ao = (uint32_t)ao_ready();

    begin_rendering(t);
    VkCommandBuffer cmd = vk_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grass_pipeline);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &grass_pool.buffer, &zero);
    VkRect2D sc = {{0, 0}, {scaled(480), scaled(272)}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
    vkCmdSetDepthCompareOp(cmd, scene.depth_op);
    VkBool32 no = VK_FALSE;
    vk.SetColorBlendEnable(cmd, 0, 1, &no);
    VkColorComponentFlags all = 0xF;
    vk.SetColorWriteMask(cmd, 0, 1, &all);
    VkColorBlendEquationEXT eq = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                                  VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
    vk.SetColorBlendEquation(cmd, 0, 1, &eq);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cmd, bc);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdPushConstants(cmd, ge_layout, pc_stages, 0, sizeof pc, &pc);
    for (int i = 0; i < ntiles; i++) {
        /* (out of view: all its corners beyond one side) */
        int out[5] = {1, 1, 1, 1, 1};
        for (int k = 0; k < 8; k++) {
            float p[3] = {tiles[i].box[(k & 1) ? 3 : 0], tiles[i].box[(k & 2) ? 4 : 1], tiles[i].box[(k & 4) ? 5 : 2]};
            float q[4];
            for (int r = 0; r < 4; r++)
                q[r] = clip[r] * p[0] + clip[4 + r] * p[1] + clip[8 + r] * p[2] + clip[12 + r];
            out[0] &= q[0] < -q[3];
            out[1] &= q[0] > q[3];
            out[2] &= q[1] < -q[3];
            out[3] &= q[1] > q[3];
            out[4] &= q[3] <= 0;
        }
        if (out[0] || out[1] || out[2] || out[3] || out[4])
            continue;
        vkCmdDraw(cmd, 8, (uint32_t)tiles[i].count, 0, grass_slot_base(tiles[i].slot));
    }
    ge_rebind();
}
