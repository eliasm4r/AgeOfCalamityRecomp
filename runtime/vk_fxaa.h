// FXAA on the 3D picture (vk_fxaa.c), behind ACR_FXAA.
#pragma once
#include "vk.h"

typedef struct VkExec VkExec;
// vk_exec_draw.c, once a draw's fragment shader is known: FXAA runs on the output before the game's copy out of the
// picture before the HUD (the draw then goes on); 1 would skip the draw.
int vk_fxaa_draw(VkExec *ex, int32_t count);
