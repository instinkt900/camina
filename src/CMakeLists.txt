add_library(engine_core STATIC
    core/log.cpp
    core/jobs.cpp
    core/arena.cpp
    core/frame_stats.cpp
    core/timestep.cpp
    core/guid.cpp
    assets/content.cpp
    assets/hot_reload.cpp
    assets/irradiance.cpp
    assets/manifest.cpp
    assets/material.cpp
    assets/meta.cpp
    assets/reference.cpp
    assets/mesh.cpp
    assets/shader.cpp
    assets/texture.cpp
    platform/window.cpp
    platform/input.cpp
    platform/paths.cpp
    platform/process.cpp
    platform/watch.cpp
    gfx/types.cpp
    reflect/registry.cpp
    reflect/inspector.cpp
    editor/camera_lines.cpp
    editor/edits.cpp
    editor/interaction.cpp
    editor/fly_camera.cpp
    editor/history.cpp
    editor/panels.cpp
    editor/picking.cpp
    editor/placement.cpp
    editor/play_mode.cpp
    editor/viewport.cpp
    gfx/vulkan/vk_device.cpp
    gfx/vulkan/vk_swapchain.cpp
    gfx/vulkan/vk_frame.cpp
    gfx/vulkan/vk_pipeline.cpp
    gfx/vulkan/vk_resources.cpp
    gfx/vulkan/vk_imgui.cpp
    gfx/vulkan/vk_vma.cpp
    render/material_cache.cpp
    render/mesh_cache.cpp
    render/mesh_pass.cpp
    render/shadow_pass.cpp
    render/render_graph.cpp
    render/shader_bindings.cpp
    render/texture_cache.cpp
    render/scene_renderer.cpp
    render/tonemap_pass.cpp
    render/debug_line_pass.cpp
    physics/components.cpp
    physics/conventions.cpp
    physics/simulation.cpp
    physics/world.cpp
    play/session.cpp
    scene/camera.cpp
    scene/step_motion.cpp
    scene/world.cpp
    scene/component_registry.cpp
    scene/scene_file.cpp
    scene/prefab.cpp
    scene/references.cpp
)

# M8. The script host and the component that names a script. These join
# engine_core rather than becoming a target of their own, the way src/ui does,
# because script/host.h names no sol2 type and so nothing leaks out. Lua and
# sol2 stay PRIVATE below for the same reason.
if(ENGINE_WITH_LUA)
    target_sources(engine_core PRIVATE
        script/bindings.cpp
        script/components.cpp
        script/host.cpp
    )
endif()

# M11. The audio device. These join engine_core the way the script host does,
# because audio/device.h names no miniaudio type and so nothing leaks out.
# miniaudio stays a PRIVATE link below for the same reason, and
# scripts/check-miniaudio-containment.sh keeps it inside this directory.
if(ENGINE_WITH_AUDIO)
    target_sources(engine_core PRIVATE
        audio/device.cpp
        audio/miniaudio_impl.cpp
    )

    # miniaudio carries its own implementation, and exactly one translation unit
    # asks for it. That unit holds nothing else, so the warning set and
    # clang-tidy stay off it without covering any of our own code. This is the
    # same treatment gfx/vulkan/vk_vma.cpp gets.
    set_source_files_properties(audio/miniaudio_impl.cpp PROPERTIES
        SKIP_LINTING ON
        COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
    )
endif()

# The shaders are cooked content now, not generated source. tools/cooker builds
# them into ENGINE_COOKED_CONTENT_DIR, and render/cube_pass.cpp reads them from
# there through assets::Content.

# VMA is one large third-party header, and rule 4.4 says we do not patch it.
# Compiling it under the engine warning set and clang-tidy reports problems in
# code we will never change, so both are off for this file alone.
set_source_files_properties(gfx/vulkan/vk_vma.cpp PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)

# The ImGui SDL3 and Vulkan backends, and the std::string helper. They live in
# the Conan package, not in this tree. See cmake/ImGuiBackends.cmake.
engine_add_imgui_backends(engine_core)

add_library(engine::core ALIAS engine_core)

target_include_directories(engine_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(engine_core
    PUBLIC
        glm::glm
        spdlog::spdlog
        Tracy::TracyClient
        enkiTS::enkiTS
        EnTT::EnTT
        # reflect/json.h is a public header and includes the JSON parser.
        nlohmann_json::nlohmann_json
    PRIVATE
        SDL3::SDL3
        # Box3D stays private, so its include directory reaches src/physics/ and
        # nothing else. That is the first half of the containment rule, and
        # scripts/check-box3d-containment.sh is the other half.
        box3d
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        # reflect/inspector.h names no ImGui type, so this stays private. An
        # application that calls ImGui itself links imgui::imgui as well.
        imgui::imgui
)

# M8. Lua and sol2 reach script/host.cpp and nothing else. The header holds an
# opaque pointer, so no consumer of engine::core sees a sol2 type or needs its
# include directory. ENGINE_WITH_LUA is PUBLIC, because an application has to
# know whether the host exists before it calls one.
if(ENGINE_WITH_LUA)
    target_link_libraries(engine_core PRIVATE lua::lua sol2::sol2)
    target_compile_definitions(engine_core PUBLIC ENGINE_WITH_LUA)
endif()

# M11. miniaudio reaches audio/device.cpp and audio/miniaudio_impl.cpp and
# nothing else. The header holds an interface with no miniaudio type in it, so
# no consumer of engine::core sees one or needs its include directory.
# ENGINE_WITH_AUDIO is PUBLIC, because an application has to know whether a
# device can be created before it asks for one.
if(ENGINE_WITH_AUDIO)
    target_link_libraries(engine_core PRIVATE miniaudio::miniaudio)
    target_compile_definitions(engine_core PUBLIC ENGINE_WITH_AUDIO)
endif()

# The math conventions in DESIGN.md section 3. These must reach every translation
# unit that includes glm, so they are PUBLIC. math/conventions.h checks that they
# are set and fails the build if they are not.
target_compile_definitions(engine_core PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_FORCE_QUAT_DATA_WXYZ
    GLM_ENABLE_EXPERIMENTAL
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
    # ENGINE_ASSERT stays active everywhere except a Release build.
    # ENGINE_CHECK is always active. See src/core/assert.h.
    $<$<NOT:$<CONFIG:Release>>:ENGINE_ENABLE_ASSERTS>
)

# windows.h defines min and max as macros, which breaks std::min and std::max. It
# also pulls in a large amount that no part of the engine uses. Set these before
# anything reaches windows.h through a dependency header.
if(WIN32)
    target_compile_definitions(engine_core PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# MSVC keeps its old preprocessor unless you ask for the conforming one, and the
# old one has no __VA_OPT__. reflect/reflect.h needs it for ENGINE_FIELD, and
# that header is public, so this is PUBLIC too.
if(MSVC)
    target_compile_options(engine_core PUBLIC /Zc:preprocessor)
endif()

engine_set_warnings(engine_core)
engine_enable_clang_tidy(engine_core)
