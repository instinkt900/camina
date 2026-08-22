// miniaudio ships as a header that carries its own implementation, and exactly
// one translation unit must ask for it. This is that unit, and it holds nothing
// else, so the warning set and clang-tidy can stay off it without covering any
// of our own code. gfx/vulkan/vk_vma.cpp does the same for VMA.
//
// See src/CMakeLists.txt for the properties this file carries.

#include "audio/miniaudio_config.h"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
