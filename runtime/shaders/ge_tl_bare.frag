#version 450
// ge_tl.frag where a pixel cannot count which triangle of its draw it is in (the device has no tessellation): no blood
// run down the animals
#extension GL_EXT_fragment_shader_barycentric : require
#define TL_CORNERS
#define TL_WOUNDS
#define RUN_NONE
#include "ge_frag.glsl"
