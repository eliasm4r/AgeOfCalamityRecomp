// FXAA on the 3D picture (vk_fxaa.c), behind ACR_FXAA.
#pragma once
#include "vk.h"

typedef struct VkExec VkExec;
// vk_exec_draw.c, once a draw's fragment shader is known: 1 when FXAA was drawn in its place (the draw is skipped).
int vk_fxaa_draw(VkExec *ex, int32_t count);
