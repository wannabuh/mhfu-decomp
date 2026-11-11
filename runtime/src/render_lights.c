/* Light sets (render_internal.h): what the GE's lights are for each draw lit by the pixel, in a storage buffer - half of
 * it each frame in flight - a draw naming its set; the same buffer holds cameras (the tessellated draws', the grass's)
 * and the point lights.
 *
 * Point lights: the game has none. What it draws adding to the picture in the world (fire, glows) is a light: each such
 * draw's vertices give a place and a colour (with its texture's average colour), those near each other are one light,
 * and the warm ones light the next frame around them (MHFU_POINT_LIGHTS=0: none; MHFU_POINT_GAIN: their strength) */
#include "render_internal.h"

#include "hle.h"

#include <stdlib.h>
#include <string.h>

#define MAX_EMITTERS 1024
#define MAX_POINT_LIGHTS 8
#define POINT_LIGHT_REACH 4000.0f       /* lights this far from what the camera looks at, at most */
#define POINT_LIGHT_RADIUS 900.0f       /* how far a light reaches */
#define POINT_LIGHT_GAIN 1.0f
#define LIGHT_FIRE_AGE 2.0f             /* seconds a light is seen before it is a fire's (slow to fade) */

VkBuf light_buf;
uint32_t light_sets_used;
int point_lights_on = -1, emitting;
static uint32_t prim_lights = ~0u;
static LightSet last_lights;
static uint32_t camera_set = ~0u;       /* this frame's set holding camera_clip */
static float camera_clip[16];
static float point_gain = POINT_LIGHT_GAIN;
typedef struct { float pos[3], color[3], weight; } Emitter;
static Emitter emitters[MAX_EMITTERS];
static int nemitters;
static double emit_sum[7];          /* the draw being batched: weight, weighted position and colour */
static LightSet point_lights[2];    /* the lights for the frame being made (two sets of four) */
static int point_shadows_next;      /* how many of them have shadows */

void lights_init(void) {
    light_buf = vk_buffer(2 * LIGHT_SETS * sizeof(LightSet), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
    VkDescriptorBufferInfo lbi = {light_buf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet lw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ge_set, BIND_LIGHTS, 0, 1,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &lbi, NULL};
    vkUpdateDescriptorSets(vk.device, 1, &lw, 0, NULL);
}

void lights_settings(void) {
    point_lights_on = !getenv("MHFU_POINT_LIGHTS") || atoi(getenv("MHFU_POINT_LIGHTS")) != 0;
    if (getenv("MHFU_POINT_GAIN"))
        point_gain = (float)atof(getenv("MHFU_POINT_GAIN"));
}

uint32_t prim_light_set(void) {
    return prim_lights;
}

uint32_t point_light_set(void) {
    return (uint32_t)vk.frame * LIGHT_SETS + LIGHT_SETS - 2;
}

const LightSet *lights_points(void) {
    return point_lights;
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
    if (sun >= 0 && sun < n)
        memcpy(scene.sun_rgb, colors + 4 * sun, sizeof scene.sun_rgb);
    if (prim_lights != ~0u && light_sets_used && !memcmp(&l, &last_lights, sizeof l))
        return;
    if (light_sets_used >= BLOOD_SET_AT) {     /* (the last: the blood's, the point lights') */
        prim_lights = ~0u;
        return;
    }
    uint32_t index = (uint32_t)vk.frame * LIGHT_SETS + light_sets_used++;
    memcpy((LightSet *)light_buf.map + index, &l, sizeof l);
    last_lights = l;
    prim_lights = index;
}

/* A vertex of the primitive being batched, if it adds to the picture (emitting): its light, gathered */
void lights_emit(const float *w, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    double wgt = (double)(r + g + b) * (double)a / (3.0 * 255.0 * 255.0);
    emit_sum[0] += wgt;
    emit_sum[1] += wgt * w[0];
    emit_sum[2] += wgt * w[1];
    emit_sum[3] += wgt * w[2];
    emit_sum[4] += wgt * r / 255.0;
    emit_sum[5] += wgt * g / 255.0;
    emit_sum[6] += wgt * b / 255.0;
}

/* The batch's light, if it adds to the picture: an emitter, its colour the vertices' times the texture's */
void lights_flush_emitter(uint32_t tex_slot) {
    if (emit_sum[0] > 0 && nemitters < MAX_EMITTERS) {
        Emitter *e = &emitters[nemitters++];
        double w = emit_sum[0];
        for (int k = 0; k < 3; k++) {
            e->pos[k] = (float)(emit_sum[1 + k] / w);
            e->color[k] = (float)(emit_sum[4 + k] / w);
            if (tex_slot < MAX_TEXTURES)
                e->color[k] *= tex_average[tex_slot][k];
        }
        e->weight = (float)w;
    }
    memset(emit_sum, 0, sizeof emit_sum);
}

/* How much a light matters: how bright it is, less the further it is from what the camera looks at */
static float light_rank(const float *pos, float power) {
    float d = sqrtf((pos[0] - scene.focus[0]) * (pos[0] - scene.focus[0]) + (pos[1] - scene.focus[1]) * (pos[1] - scene.focus[1]) +
                    (pos[2] - scene.focus[2]) * (pos[2] - scene.focus[2]));
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
     * a light found again near where one was eases to its new colour and strength, one gone fades out (below) */
    typedef struct { float pos[3], color[3], strength, power, age; int seen; } Smoothed;
    static Smoothed lit[MAX_POINT_LIGHTS];
    static uint64_t last_us;
    uint64_t now_us = time_us();
    float dt = last_us && now_us > last_us ? fminf((float)(now_us - last_us) / 1e6f, 0.1f) : 0.0f;
    last_us = now_us;
    for (int i = 0; i < MAX_POINT_LIGHTS; i++)
        lit[i].seen = 0;
    for (int j = 0; j < n; j++) {
        const float *c = l[j].color;
        float dc = sqrtf((l[j].pos[0] - scene.focus[0]) * (l[j].pos[0] - scene.focus[0]) +
                         (l[j].pos[1] - scene.focus[1]) * (l[j].pos[1] - scene.focus[1]) +
                         (l[j].pos[2] - scene.focus[2]) * (l[j].pos[2] - scene.focus[2]));
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
            lit[k].age = 0;
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
        lit[k].age += dt;
        lit[k].seen = 1;
    }
    memset(point_lights, 0, sizeof point_lights);
    /* one gone: a fire out of view fades by half in ten seconds (the game draws no flames there, they are still
     * burning); one in view, or a flash, at once */
    float clip[16];
    int have_clip = world_to_clip(scene.view, scene.world_view, clip);
    for (int i = 0; i < MAX_POINT_LIGHTS; i++)
        if (!lit[i].seen) {
            int in_view = 0;
            if (have_clip) {
                float c[4];
                for (int r = 0; r < 4; r++)
                    c[r] = clip[r] * lit[i].pos[0] + clip[4 + r] * lit[i].pos[1] + clip[8 + r] * lit[i].pos[2] + clip[12 + r];
                in_view = c[3] > 0 && fabsf(c[0]) <= c[3] && fabsf(c[1]) <= c[3];
            }
            lit[i].strength *= lit[i].age >= LIGHT_FIRE_AGE && !in_view ? 0.9995f : expf(-dt / 0.1f);
        }
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
    point_lights[0].pad0 = (uint32_t)point_shadows_next;
    nemitters = 0;
}

/* The light set holding the world to the clip space (as put makes it: x, y over half the target, less w), for the
 * tessellated draws; ~0 if there is no room */
uint32_t camera_light_set(void) {
    float clip[16];
    if (!world_to_clip(prim_view, prim_world_view, clip))
        return ~0u;
    if (camera_set != ~0u && !memcmp(clip, camera_clip, sizeof clip))
        return camera_set;
    if (light_sets_used >= BLOOD_SET_AT)
        return ~0u;
    camera_set = (uint32_t)vk.frame * LIGHT_SETS + light_sets_used++;
    memcpy(camera_clip, clip, sizeof clip);
    LightSet l;
    memset(&l, 0, sizeof l);
    memcpy(l.dir, clip, sizeof clip);
    memcpy((LightSet *)light_buf.map + camera_set, &l, sizeof l);
    return camera_set;
}

/* gpu.c: the blood on the ground (shared.h BLOOD_SET_AT), each frame it records. Kept, and put in both halves of the
 * sets at each frame's end: the pixel stage reads them at the one place whichever frame is in flight. */
_Static_assert(BLOOD_SET_AT == LIGHT_SETS - 2 - BLOOD_SETS, "the blood's sets: before the point lights'");
static LightSet blood_sets[BLOOD_SETS];

void render_blood(int n, const float (*at)[4], const float (*color)[4], const float (*lie)[4], int ndrops,
                  const float (*drop_at)[4], const float (*drop_color)[4], const float (*drop_streak)[4], const float *eye) {
    LightSet l[BLOOD_SETS];
    memset(l, 0, sizeof l);
    if (n > 4 * BLOOD_MARK_SETS)
        n = 4 * BLOOD_MARK_SETS;
    if (ndrops > 4 * BLOOD_DROP_SETS)
        ndrops = 4 * BLOOD_DROP_SETS;
    for (int i = 0; i < ndrops; i++) {
        memcpy(l[BLOOD_DROP_AT + i / 4].dir[i % 4], drop_at[i], 16);
        memcpy(l[BLOOD_DROP_AT + i / 4].color[i % 4], drop_color[i], 16);
        memcpy(l[BLOOD_DROP_AT + BLOOD_DROP_SETS + i / 4].dir[i % 4], drop_streak[i], 16);
    }
    l[BLOOD_DROP_AT].count = (uint32_t)ndrops;
    memcpy(l[BLOOD_SETS - 1].dir[0], eye, 12);
    for (int i = 0; i < n; i++) {
        memcpy(l[i / 4].dir[i % 4], at[i], 16);
        memcpy(l[i / 4].color[i % 4], color[i], 16);
        memcpy(l[BLOOD_MARK_SETS + i / 4].dir[i % 4], lie[i], 16);
    }
    l[0].count = (uint32_t)n;
    memcpy(blood_sets, l, sizeof l);    /* (read by the renderer's thread as it is: a mark a frame late is nothing) */
}

/* The frame's end: the last emitter's light, the point lights for the next frame (its half: vk.frame is the next's
 * now); the sets begun again. How many point lights have shadows next frame */
int lights_end_frame(uint32_t tex_slot) {
    if (emitting)
        lights_flush_emitter(tex_slot);
    emitting = 0;
    make_point_lights();
    if (point_lights_on > 0)
        memcpy((LightSet *)light_buf.map + point_light_set(), point_lights, sizeof point_lights);
    for (uint32_t half = 0; half < 2; half++)
        memcpy((LightSet *)light_buf.map + half * LIGHT_SETS + BLOOD_SET_AT, blood_sets, sizeof blood_sets);
    light_sets_used = 0;
    camera_set = ~0u;
    prim_lights = ~0u;
    return point_lights_on > 0 ? point_shadows_next : 0;
}
