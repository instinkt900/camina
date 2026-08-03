# The game module. It links into the runtime today and into the editor at M8.
#
# Rule 4.3 says the editor is an application over engine_core rather than a
# build mode, so the game has to compile into both. Building it as a library
# from the start is what keeps that true.
add_library(sandbox STATIC
    game.cpp
)

add_library(camina::sandbox ALIAS sandbox)

target_include_directories(sandbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(sandbox PUBLIC engine::core)

# The game reads cooked content from next to the executable now. The cooker
# builds sandbox/content into it, and tools/cooker/CMakeLists.txt wires that up.

engine_set_warnings(sandbox)
engine_enable_clang_tidy(sandbox)
