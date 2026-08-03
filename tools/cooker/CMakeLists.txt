# The asset cooker. DESIGN.md section 6 makes it a separate executable, so a
# cook runs on a build machine with no graphics driver.
#
# The logic is a library and main.cpp is only the command line, so a test can
# cook a tree and check what came out.
add_library(cooker_lib STATIC
    cook.cpp
    material.cpp
    mesh.cpp
    prefab.cpp
    texture.cpp
    stb_image_impl.cpp
)

# stb_image carries its own implementation, and this file asks for it. It holds
# nothing of ours, so the engine warning set and clang-tidy stay off it. Rule
# 4.4 keeps the file in the Conan cache rather than in third_party/, because we
# do not patch it.
set_source_files_properties(stb_image_impl.cpp PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)

target_include_directories(cooker_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# engine::core brings the GUID, the sidecar, and the manifest. It brings no
# Vulkan into this target, because engine_core keeps volk PRIVATE.
#
# stb and bc7enc are PRIVATE. They are how this target reads an image and
# compresses it, and no consumer of the cooker needs either one.
target_link_libraries(cooker_lib
    PUBLIC
        engine::core
    PRIVATE
        stb::stb
        bc7enc
        cgltf::cgltf
        meshoptimizer::meshoptimizer
)

engine_set_warnings(cooker_lib)
engine_enable_clang_tidy(cooker_lib)

add_executable(cooker main.cpp)
target_link_libraries(cooker PRIVATE cooker_lib)

engine_set_warnings(cooker)
engine_enable_clang_tidy(cooker)

# glslc compiles the shaders. It arrives through the shaderc tool requirement
# in conanfile.py, and the same find_program the deleted cmake/Shaders.cmake
# used now only tells the cooker where it is.
find_program(ENGINE_GLSLC_EXE NAMES glslc)
if(NOT ENGINE_GLSLC_EXE)
    message(FATAL_ERROR
        "glslc was not found. Run conan install first, and make sure the "
        "generated build environment is active.")
endif()
message(STATUS "Using glslc at ${ENGINE_GLSLC_EXE}")

# Cooks one source tree into the directory the executables run from.
#
# The output goes beside the executables rather than under a build path,
# because platform::cooked_content_root() looks next to the running program.
# One cook therefore serves the runtime, the editor, and the tests.
#
# The stamp lives inside the cooked directory rather than in the build tree, so
# the two cannot drift. Deleting the cooked content deletes the stamp with it,
# and the next build cooks again. A stamp left behind in the build tree would
# stay newer than every input, the build would skip the cooker, and a program
# would start with nothing to draw.
#
# name:   the subdirectory under the cooked root, so two trees stay apart.
# source: the source content directory.
function(engine_cook_content name source)
    set(out_dir "${ENGINE_COOKED_CONTENT_DIR}/${name}")
    set(stamp "${out_dir}/.cooked")

    file(GLOB_RECURSE cook_inputs CONFIGURE_DEPENDS "${source}/*")

    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND cooker
                --content "${source}"
                --out "${out_dir}"
                --glslc "${ENGINE_GLSLC_EXE}"
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
        DEPENDS cooker ${cook_inputs}
        COMMENT "Cooking ${name} content"
        VERBATIM
    )

    add_custom_target(content_${name} ALL DEPENDS "${stamp}")
endfunction()

# The engine tree holds the shaders and the textures that render/ reads. The
# game tree holds the scene and the prefabs that sandbox/ reads.
engine_cook_content(engine "${CMAKE_SOURCE_DIR}/src/render/content")
engine_cook_content(game "${CMAKE_SOURCE_DIR}/sandbox/content")
