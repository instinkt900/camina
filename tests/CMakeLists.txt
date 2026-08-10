# M0 keeps the test setup minimal. A test framework arrives when there is
# something that needs one. See rule 4.6 in DESIGN.md.

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE engine::core)
engine_set_warnings(test_core)
engine_enable_clang_tidy(test_core)

add_test(NAME core COMMAND test_core)

add_executable(test_reflect test_reflect.cpp)
target_link_libraries(test_reflect PRIVATE engine::core)
engine_set_warnings(test_reflect)
engine_enable_clang_tidy(test_reflect)

add_test(NAME reflect COMMAND test_reflect)

add_executable(test_json test_json.cpp)
target_link_libraries(test_json PRIVATE engine::core)
engine_set_warnings(test_json)
engine_enable_clang_tidy(test_json)

add_test(NAME json COMMAND test_json)

add_executable(test_scene test_scene.cpp)
target_link_libraries(test_scene PRIVATE engine::core)
engine_set_warnings(test_scene)
engine_enable_clang_tidy(test_scene)

add_test(NAME scene COMMAND test_scene)

add_executable(test_scene_file test_scene_file.cpp)
target_link_libraries(test_scene_file PRIVATE engine::core)
engine_set_warnings(test_scene_file)
engine_enable_clang_tidy(test_scene_file)

add_test(NAME scene_file COMMAND test_scene_file)

add_executable(test_prefab test_prefab.cpp)
target_link_libraries(test_prefab PRIVATE engine::core)
engine_set_warnings(test_prefab)
engine_enable_clang_tidy(test_prefab)

add_test(NAME prefab COMMAND test_prefab)

add_executable(test_guid test_guid.cpp)
target_link_libraries(test_guid PRIVATE engine::core)
engine_set_warnings(test_guid)
engine_enable_clang_tidy(test_guid)

add_test(NAME guid COMMAND test_guid)

# The cooker. This links cooker_lib rather than the executable, so a test can
# cook a tree in a temporary directory and read the counts back.
add_executable(test_cooker test_cooker.cpp)
target_link_libraries(test_cooker PRIVATE cooker_lib)
engine_set_warnings(test_cooker)
engine_enable_clang_tidy(test_cooker)

# The M4.5 tests run the cooker executable, because that is how hot reload
# cooks. Linking cooker_lib does not build it, and both land in the same
# output directory, so building this target alone would otherwise leave a test
# looking for a program that is not there yet.
add_dependencies(test_cooker cooker)

add_test(NAME cooker COMMAND test_cooker)

# The glTF importer and the cooked mesh format. This links cooker_lib for the
# same reason test_cooker does.
add_executable(test_mesh test_mesh.cpp)
target_link_libraries(test_mesh PRIVATE cooker_lib)
engine_set_warnings(test_mesh)
engine_enable_clang_tidy(test_mesh)

add_test(NAME mesh COMMAND test_mesh)

# The cooked shader format. It links engine::core rather than cooker_lib,
# because the format is what the two sides agree on and the runtime is one of
# them. The reflection that fills it needs glslc, so it is tested in
# test_cooker.cpp instead.
add_executable(test_shader test_shader.cpp)
target_link_libraries(test_shader PRIVATE engine::core)
engine_set_warnings(test_shader)
engine_enable_clang_tidy(test_shader)

add_test(NAME shader COMMAND test_shader)

# The half-float converter the cooker uses for environments and the BRDF table.
# This links cooker_lib for the include path to to_half.h.
add_executable(test_half test_half.cpp)
target_link_libraries(test_half PRIVATE cooker_lib)
engine_set_warnings(test_half)
engine_enable_clang_tidy(test_half)

add_test(NAME half COMMAND test_half)

# The file watcher and the process runner. This program is also its own child,
# so the process tests have something to run on both platforms.
add_executable(test_platform test_platform.cpp)
target_link_libraries(test_platform PRIVATE engine::core)
engine_set_warnings(test_platform)
engine_enable_clang_tidy(test_platform)

add_test(NAME platform COMMAND test_platform)

add_executable(test_assets test_assets.cpp)
target_link_libraries(test_assets PRIVATE engine::core)
engine_set_warnings(test_assets)
engine_enable_clang_tidy(test_assets)

add_test(NAME assets COMMAND test_assets)

add_executable(test_render test_render.cpp)
target_link_libraries(test_render PRIVATE engine::core)
engine_set_warnings(test_render)
engine_enable_clang_tidy(test_render)

add_test(NAME render COMMAND test_render)

# The barrier derivation. It needs no device at all, which is the point of
# declaring what a pass touches as data. See DESIGN.md section 9.
add_executable(test_render_graph test_render_graph.cpp)
target_link_libraries(test_render_graph PRIVATE engine::core)
engine_set_warnings(test_render_graph)
engine_enable_clang_tidy(test_render_graph)

add_test(NAME render_graph COMMAND test_render_graph)

# The frustum planes and the sphere test. Also no device, for the same reason.
# A sign error in one plane culls everything or nothing, and neither failure
# points back at the arithmetic. See issue #98.
add_executable(test_frustum test_frustum.cpp)
target_link_libraries(test_frustum PRIVATE engine::core)
engine_set_warnings(test_frustum)
engine_enable_clang_tidy(test_frustum)

add_test(NAME frustum COMMAND test_frustum)

# The world sphere a mesh bound turns into. No device either. This is the step
# before the frustum test, and it is where a radius that comes out too small
# drops a mesh that is on screen. See issue #176.
add_executable(test_bounds test_bounds.cpp)
target_link_libraries(test_bounds PRIVATE engine::core)
engine_set_warnings(test_bounds)
engine_enable_clang_tidy(test_bounds)

add_test(NAME bounds COMMAND test_bounds)

# The game module. This test reads the content the sandbox actually ships, so a
# broken prefab or scene fails here rather than in the running application.
add_executable(test_sandbox test_sandbox.cpp)
target_link_libraries(test_sandbox PRIVATE camina::sandbox)
engine_set_warnings(test_sandbox)
engine_enable_clang_tidy(test_sandbox)

add_test(NAME sandbox COMMAND test_sandbox)

# The EnTT preconditions reach ENGINE_ASSERT through src/core/entt.h. This
# program reads a component the entity does not carry, so it is expected to die.
# It passes only when the engine assertion reported the failure first. Without
# the wiring the process dies just the same, with no message, so the message is
# the whole test.
#
# ENGINE_ASSERT compiles out in a Release build, so the test is not registered
# there. CI and the presets build RelWithDebInfo.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    add_executable(test_entt_assert test_entt_assert.cpp)
    target_link_libraries(test_entt_assert PRIVATE engine::core)
    engine_set_warnings(test_entt_assert)
    engine_enable_clang_tidy(test_entt_assert)

    # expect_assert.cmake runs the program and checks the message. A ctest
    # property cannot do this. See the comment at the top of that script.
    add_test(
        NAME entt_assert
        COMMAND ${CMAKE_COMMAND}
                -DEXE=$<TARGET_FILE:test_entt_assert>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_assert.cmake
    )
endif()

# Rule 4.1. No file outside src/gfx/vulkan/ can include a Vulkan header.
#
# The check is a shell script, so it needs bash. A Windows machine without Git
# Bash does not get the test. The vulkan-containment job in CI runs on Linux and
# covers every push, so the rule still holds.
find_program(BASH_EXE bash)
if(BASH_EXE)
    add_test(
        NAME vulkan_containment
        COMMAND ${CMAKE_COMMAND} -E env ${BASH_EXE}
                ${CMAKE_SOURCE_DIR}/scripts/check-vulkan-containment.sh
                ${CMAKE_SOURCE_DIR}
    )
else()
    message(STATUS "bash not found, skipping the vulkan_containment test")
endif()

# M6.2. The moth_ui batching recorder. It names no Vulkan type and opens no
# device, so the state stack and the batching decision are testable directly.
# Only built when the game UI is on, because the target it links does not exist
# otherwise.
if(ENGINE_WITH_UI)
    add_executable(test_ui_renderer test_ui_renderer.cpp)
    target_link_libraries(test_ui_renderer PRIVATE engine::ui)
    engine_set_warnings(test_ui_renderer)
    engine_enable_clang_tidy(test_ui_renderer)

    add_test(NAME ui_renderer COMMAND test_ui_renderer)
endif()
