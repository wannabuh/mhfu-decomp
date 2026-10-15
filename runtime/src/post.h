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
} PostFrame;

/* make: render.c's pipelines (a full-screen pass: blend 0 none, 1 adding) */
typedef VkPipeline (*PostPipeline)(VkShaderModule fs, VkPipelineLayout layout, VkFormat color, int blend);

int post_init(int scale, VkImageView shadow_view, VkSampler shadow_sampler, PostPipeline make);
int post_on(void);
VkImg *post_glow(void);         /* where render.c draws the light the effects add (fw x fh, cleared by it) */
void post_apply(VkImg *color, VkImg *depth, const PostFrame *f);
void post_end_frame(void);

#endif
