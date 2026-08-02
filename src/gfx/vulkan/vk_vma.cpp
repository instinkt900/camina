// The single translation unit that compiles the Vulkan Memory Allocator.
//
// VMA ships as one large header. Exactly one source file defines
// VMA_IMPLEMENTATION, and this is that file. src/CMakeLists.txt turns off the
// engine warning set and clang-tidy for this file only, because the code is a
// third-party dependency and we do not patch it. See rule 4.4 in DESIGN.md.

// volk must come first. It declares every Vulkan symbol that VMA then uses.
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
