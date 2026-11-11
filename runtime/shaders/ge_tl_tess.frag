#version 450
// ge.frag for the GPU's vertex stage (MHFU_GPU_TL) tessellated (ge_tl.tesc, ge_tl.tese: the triangles are not the
// game's, so no corners; which of the game's each is, the stages before say) - with its wounds
#define TL_WOUNDS
#define RUN_PATCH
#include "ge_frag.glsl"
