# M0 keeps the test setup minimal. A test framework arrives when there is
# something that needs one. See rule 4.6 in DESIGN.md.

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE engine::core)
engine_set_warnings(test_core)
engine_enable_clang_tidy(test_core)

add_test(NAME core COMMAND test_core)

add_executable(test_timestep test_timestep.cpp)
target_link_libraries(test_timestep PRIVATE engine::core)
engine_set_warnings(test_timestep)
engine_enable_clang_tidy(test_timestep)

add_test(NAME timestep COMMAND test_timestep)

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

# The cooker. This links engine::import rather than the executable, so a test can
# cook a tree in a temporary directory and read the counts back.
add_executable(test_cooker test_cooker.cpp)
target_link_libraries(test_cooker PRIVATE engine::import)
engine_set_warnings(test_cooker)
engine_enable_clang_tidy(test_cooker)

# The M4.5 tests run the cooker executable, because that is how hot reload
# cooks. Linking engine::import does not build it, and both land in the same
# output directory, so building this target alone would otherwise leave a test
# looking for a program that is not there yet.
add_dependencies(test_cooker cooker)

add_test(NAME cooker COMMAND test_cooker)

# The glTF importer and the cooked mesh format. This links engine::import for the
# same reason test_cooker does.
add_executable(test_mesh test_mesh.cpp)
target_link_libraries(test_mesh PRIVATE engine::import)
engine_set_warnings(test_mesh)
engine_enable_clang_tidy(test_mesh)

add_test(NAME mesh COMMAND test_mesh)

# The cooked shader format. It links engine::core rather than engine::import,
# because the format is what the two sides agree on and the runtime is one of
# them. The reflection that fills it needs glslc, so it is tested in
# test_cooker.cpp instead.
add_executable(test_shader test_shader.cpp)
target_link_libraries(test_shader PRIVATE engine::core)
engine_set_warnings(test_shader)
engine_enable_clang_tidy(test_shader)

add_test(NAME shader COMMAND test_shader)

# The half-float converter the cooker uses for environments and the BRDF table.
# This links engine::import for the include path to to_half.h.
add_executable(test_half test_half.cpp)
target_link_libraries(test_half PRIVATE engine::import)
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

# The input module. This opens no window, because sample() is the only part
# that needs one and it is a free function the test never calls.
add_executable(test_input test_input.cpp)
target_link_libraries(test_input PRIVATE engine::core)
engine_set_warnings(test_input)
engine_enable_clang_tidy(test_input)

add_test(NAME input COMMAND test_input)

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

# M7.1. Box3D builds, links, and agrees with the engine about which way is up.
# The value comes out of the library at run time, so a Box3D update that turns
# the world over fails here rather than in the game. See DESIGN.md section 3.
add_executable(test_step_motion test_step_motion.cpp)
target_link_libraries(test_step_motion PRIVATE engine::core)
engine_set_warnings(test_step_motion)
engine_enable_clang_tidy(test_step_motion)
add_test(NAME step_motion COMMAND test_step_motion)

add_executable(test_physics test_physics.cpp)
target_link_libraries(test_physics PRIVATE engine::core)
engine_set_warnings(test_physics)
engine_enable_clang_tidy(test_physics)

add_test(NAME physics COMMAND test_physics)

# M8.1. The Lua host. Every script here is a string literal, so this needs no
# cooked tree and no content root. It only builds when the option is on,
# because with it off engine_core holds no host at all.
if(ENGINE_WITH_LUA)
    add_executable(test_script test_script.cpp)
    target_link_libraries(test_script PRIVATE engine::core)
    engine_set_warnings(test_script)
    engine_enable_clang_tidy(test_script)

    add_test(NAME script COMMAND test_script)

    # M10.7a. The fixed step, and what a pause does to it. It drives a real
    # session with a faked UI surface, so it needs no window and no layout file.
    add_executable(test_session test_session.cpp)
    target_link_libraries(test_session PRIVATE engine::core)
    engine_set_warnings(test_session)
    engine_enable_clang_tidy(test_session)

    add_test(NAME session COMMAND test_session)
endif()

# The game module. This test reads the content the sandbox actually ships, so a
# broken prefab or scene fails here rather than in the running application.
# M12.1. The undo stack. It is driven with an edit that counts its own calls, so
# it needs no world, no window, and no real edit to exist yet.
add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE engine::core)
engine_set_warnings(test_history)
engine_enable_clang_tidy(test_history)

add_test(NAME history COMMAND test_history)

# M12.3. The component edits. Each one is a document and an identity, so this
# drives a bare World with no window and no device.
add_executable(test_edits test_edits.cpp)
target_link_libraries(test_edits PRIVATE engine::core)
engine_set_warnings(test_edits)
engine_enable_clang_tidy(test_edits)

add_test(NAME edits COMMAND test_edits)

add_executable(test_interaction test_interaction.cpp)
target_link_libraries(test_interaction PRIVATE engine::core)
engine_set_warnings(test_interaction)
engine_enable_clang_tidy(test_interaction)

add_test(NAME interaction COMMAND test_interaction)

# M9.5a. The camera the scene plays through, and the fly camera that steers one.
# Neither opens a device, so this runs anywhere.
add_executable(test_camera test_camera.cpp)
target_link_libraries(test_camera PRIVATE engine::core)
engine_set_warnings(test_camera)
engine_enable_clang_tidy(test_camera)

add_test(NAME camera COMMAND test_camera)

# M9.4. Play-in-editor. This links the game, because the restore has to survive
# what a real scene does: bodies that fall and scripts that create entities.
add_executable(test_editor test_editor.cpp)
target_link_libraries(test_editor PRIVATE camina::sandbox)
engine_set_warnings(test_editor)
engine_enable_clang_tidy(test_editor)

add_test(NAME editor COMMAND test_editor)

add_executable(test_sandbox test_sandbox.cpp)
target_link_libraries(test_sandbox PRIVATE camina::sandbox engine::import)

# M13.4a compares a load from the cooked tree against a load from the source
# tree, so this test needs to know where the source tree is. apps/editor does
# the same thing for the same reason.
target_compile_definitions(test_sandbox PRIVATE
    ENGINE_GAME_CONTENT_SOURCE="${CMAKE_SOURCE_DIR}/sandbox/content"
)
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

# Rule 4.1. No file outside src/gfx/vulkan/ can include a Vulkan header, and the
# Box3D rule in DESIGN.md section 5 says the same of src/physics/.
#
# Two tests, because they measure different things. vulkan_containment says this
# tree obeys the rule. containment_checks says the checks can still tell. A
# check that fails open reports a clean tree either way, so the first test alone
# proves nothing about the second. See issue #231.
#
# Both are shell scripts, so they need bash. A Windows machine without Git Bash
# does not get them. The containment job in CI runs on Linux and covers every
# push, so the rule still holds.
find_program(BASH_EXE bash)
if(BASH_EXE)
    add_test(
        NAME vulkan_containment
        COMMAND ${CMAKE_COMMAND} -E env ${BASH_EXE}
                ${CMAKE_SOURCE_DIR}/scripts/check-vulkan-containment.sh
                ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME box3d_containment
        COMMAND ${CMAKE_COMMAND} -E env ${BASH_EXE}
                ${CMAKE_SOURCE_DIR}/scripts/check-box3d-containment.sh
                ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME containment_checks
        COMMAND ${CMAKE_COMMAND} -E env ${BASH_EXE}
                ${CMAKE_SOURCE_DIR}/tests/test_containment.sh
    )
else()
    message(STATUS "bash not found, skipping the containment tests")
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

    # M6.4 draws text through the recorder, so this needs the sandbox font too.
    target_compile_definitions(test_ui_renderer PRIVATE
        ENGINE_TEST_FONT_PATH="${CMAKE_SOURCE_DIR}/sandbox/content/ui/fonts/LiberationSans-Regular.ttf"
    )

    add_test(NAME ui_renderer COMMAND test_ui_renderer)

    # M10.3. Reading a cooked layout by identity. It links the importer as well,
    # because the realistic failure cases need real cooked assets of other kinds
    # rather than files a test wrote by hand.
    add_executable(test_ui_layout test_ui_layout.cpp)
    target_link_libraries(test_ui_layout PRIVATE engine::ui engine::import)
    engine_set_warnings(test_ui_layout)
    engine_enable_clang_tidy(test_ui_layout)
    add_test(NAME ui_layout COMMAND test_ui_layout)

    # M10.6. The surface a script drives the UI through. It links the importer
    # as well, because a layout has to be cooked before the surface can read it,
    # and it drives a real moth_ui node tree with no device under it.
    add_executable(test_ui_surface test_ui_surface.cpp)
    target_link_libraries(test_ui_surface PRIVATE engine::ui engine::import)
    engine_set_warnings(test_ui_surface)
    engine_enable_clang_tidy(test_ui_surface)
    add_test(NAME ui_surface COMMAND test_ui_surface)

    # M10.5. The SDL3 to moth_ui input bridge. It reads platform::InputFrame
    # rather than a device, so every case drives it with a frame written by
    # hand and no window is opened.
    add_executable(test_ui_input test_ui_input.cpp)
    target_link_libraries(test_ui_input PRIVATE engine::ui)
    engine_set_warnings(test_ui_input)
    engine_enable_clang_tidy(test_ui_input)
    add_test(NAME ui_input COMMAND test_ui_input)

    # M6.4. The font rasterizes, packs, shapes, measures and wraps with no
    # device, which is why load() is separate from upload(). The path of the
    # sandbox font comes from CMake, so the test does not guess where anything
    # is. It reads the source tree rather than the cooked tree, because the
    # cooker copies a font unchanged and a test should not need a cook first.
    add_executable(test_ui_font test_ui_font.cpp)
    target_link_libraries(test_ui_font PRIVATE engine::ui)
    target_compile_definitions(test_ui_font PRIVATE
        ENGINE_TEST_FONT_PATH="${CMAKE_SOURCE_DIR}/sandbox/content/ui/fonts/LiberationSans-Regular.ttf"
    )
    engine_set_warnings(test_ui_font)
    engine_enable_clang_tidy(test_ui_font)

    add_test(NAME ui_font COMMAND test_ui_font)

    # M6.3. RenderImage refuses a nine-sliced image rather than stretching it.
    # The refusal is an assertion, so the program is expected to die and the
    # message is the whole test. See the entt_assert entry above.
    #
    # ENGINE_ASSERT compiles out in a Release build, so this is not registered
    # there.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        add_executable(test_ui_nine_slice test_ui_nine_slice.cpp)
        target_link_libraries(test_ui_nine_slice PRIVATE engine::ui)
        engine_set_warnings(test_ui_nine_slice)
        engine_enable_clang_tidy(test_ui_nine_slice)

        add_test(
            NAME ui_nine_slice
            COMMAND ${CMAKE_COMMAND}
                    -DEXE=$<TARGET_FILE:test_ui_nine_slice>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_assert.cmake
        )
    endif()
endif()
