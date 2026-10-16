/* Post-processing of the world's picture (post.c): run by the renderer (render.c) once the world is drawn, before the
 * HUD is drawn over it. */
#ifndef POST_H
#define POST_H

#include "vk.h"

typedef struct {
    float to_view[16];      /* (PSP pixel x, y, depth, 1) to the camera's space (by columns) */
    float to_shadow[16];    /* the camera's space to the sun's shadow map (x, y -1..1, depth 0..1) */
    float sun[3];           /* the direction to the sun, in the camera's space */
    float sun_color[3];
    int shadows;            /* the shadow map was drawn this frame */
    float focus;            /* the distance to what the camera looks at (0: unknown) */
    float to_prev[16];      /* the camera's space to the last frame's PSP pixels (x, y, depth, w; by columns) */
    float to_camera[12];    /* the world to the camera's space: its rotation's rows (x, y, z, 0) */
    int moved;              /* to_prev is known (a frame before, the same camera's) */
} PostFrame;

/* make: render.c's pipelines (a full-screen pass: blend 0 none, 1 adding) */
typedef VkPipeline (*PostPipeline)(VkShaderModule fs, VkPipelineLayout layout, VkFormat color, int blend);

int post_init(float scale, VkImageView shadow_view, VkSampler shadow_sampler, PostPipeline make);
void post_rescale(float scale);   /* the internal resolution changed (the GPU idle): its images again */
int post_on(void);
int post_motion(void);       /* the motion blur is on */
float post_frame_step(void);     /* the game's frames between two frames shown */
VkImg *post_motion_image(void);  /* where render_post.c draws the hunter's, monsters' and people's motion (world: rgb; a 1) */
VkImg *post_glow(void);         /* where render_post.c draws the light the effects add (fw x fh, cleared by it) */
void post_apply(VkImg *color, VkImg *depth, const PostFrame *f);
void post_end_frame(void);

#endif
