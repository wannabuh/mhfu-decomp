#version 450
// ge.frag for the GPU's vertex stage (MHFU_GPU_TL) where a pixel cannot read its triangle's corners: with its wounds
#define TL_WOUNDS
#include "ge_frag.glsl"
