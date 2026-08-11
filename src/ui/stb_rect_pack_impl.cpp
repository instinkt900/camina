// stb_rect_pack ships as a header that carries its own implementation, and
// exactly one translation unit must ask for it. This is that unit, and it holds
// nothing else, so the warning set and clang-tidy can stay off it without
// covering any of our own code. tools/cooker/stb_image_impl.cpp does the same
// for stb_image.
//
// See src/ui/CMakeLists.txt for the properties this file carries.

#define STB_RECT_PACK_IMPLEMENTATION
#include <stb_rect_pack.h>
