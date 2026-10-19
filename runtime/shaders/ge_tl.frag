#version 450
// ge.frag for the GPU's vertex stage (MHFU_GPU_TL), its pixels reading their triangle's corners
#extension GL_EXT_fragment_shader_barycentric : require
#define TL_CORNERS
#include "ge_frag.glsl"
