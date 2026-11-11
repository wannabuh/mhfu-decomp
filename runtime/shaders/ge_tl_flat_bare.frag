#version 450
// ge_tl_flat.frag where a pixel cannot count which triangle of its draw it is in (the device has no tessellation)
#define TL_WOUNDS
#define RUN_NONE
#include "ge_frag.glsl"
