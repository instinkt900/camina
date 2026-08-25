# Every source under physics/, named once. add_library() takes it below and the
# contraction flag further down takes the same list, so a file added here is
# covered by both. Issue #229 is what the second use is for.
set(engine_physics_sources
    physics/components.cpp
    physics/conventions.cpp
    physics/simulation.cpp
    physics/world.cpp
)

add_library(engine_core STATIC
    ${engine_physics_sources}
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
    assets/sound.cpp
    audio/attenuation.cpp
    audio/bus.cpp
    audio/mix_settings.cpp
    assets/texture.cpp
    platform/window.cpp
    platform/input.cpp
    platform/paths.cpp
    platform/process.cpp
    platform/watch.cpp
    platform/watch_poll.cpp
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
    render/sky_pass.cpp
    render/render_graph.cpp
    render/shader_bindings.cpp
    render/texture_cache.cpp
    render/scene_renderer.cpp
    render/tonemap_pass.cpp
    render/debug_line_pass.cpp
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
# script/components.cpp is not in the guard below. It registers a reflected
# struct and names no Lua type, and a scene that carries a script reference has
# to read and write in a build with no interpreter. Otherwise the option changes
# which documents are valid rather than which of them run, and the cooker
# refuses the shipped game content. See src/script/components.h.
target_sources(engine_core PRIVATE
    script/components.cpp
)

if(ENGINE_WITH_LUA)
    target_sources(engine_core PRIVATE
        script/bindings.cpp
        script/host.cpp
    )
endif()

# One miniaudio implementation for each binary, and never two.
#
# miniaudio carries its own implementation and exactly one translation unit may
# ask for it. Two consumers need it now: engine_core plays sound, and
# engine_import decodes a short effect at cook time (issue #424). apps/editor
# links both, so a copy in each would be duplicate symbols at best and two
# disagreeing struct layouts at worst.
#
# It is built whatever ENGINE_WITH_AUDIO says, because a cook has to produce the
# same bytes either way. audio/miniaudio_config.h defines MA_NO_DEVICE_IO when
# the option is off, so a build with no audio compiles the decoders and no
# backend.
add_library(engine_miniaudio STATIC
    audio/miniaudio_impl.cpp
    audio/decode.cpp
)
add_library(engine::miniaudio ALIAS engine_miniaudio)

# It does not link engine::core, on purpose. engine_core links this one, and a
# cycle between two static libraries is a link order somebody has to think
# about. audio/decode.cpp needs the include path and the logger, so it takes
# those two directly.
target_include_directories(engine_miniaudio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(engine_miniaudio
    PRIVATE
        miniaudio::miniaudio
        spdlog::spdlog
        Tracy::TracyClient
)

if(ENGINE_WITH_AUDIO)
    target_compile_definitions(engine_miniaudio PRIVATE ENGINE_WITH_AUDIO)
endif()

# The implementation unit holds nothing of ours, so the warning set and
# clang-tidy stay off it without covering any of our own code. This is the same
# treatment gfx/vulkan/vk_vma.cpp gets. audio/decode.cpp is ours and keeps both.
set_source_files_properties(audio/miniaudio_impl.cpp PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)

engine_set_warnings(engine_miniaudio)
engine_enable_clang_tidy(engine_miniaudio)

# M11. The audio device. These join engine_core the way the script host does,
# because audio/device.h names no miniaudio type and so nothing leaks out.
# miniaudio stays a PRIVATE link below for the same reason, and
# scripts/check-miniaudio-containment.sh keeps it inside this directory.
if(ENGINE_WITH_AUDIO)
    target_sources(engine_core PRIVATE
        audio/device.cpp
        audio/mixer.cpp
        audio/scene_audio.cpp
        audio/script_audio.cpp
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

# Issue #229. Box3D turns contraction into a fused multiply-add off for the
# determinism its solver depends on, and third_party/CMakeLists.txt sets that
# flag on the library. The flag is PRIVATE there, so it reaches the library and
# not the callers, and the inline math in box3d/math_functions.h is compiled by
# whoever includes it. Two copies of one function that round differently give
# two answers for one input, which is the failure the flag exists to stop.
#
# **This covers the directory rather than the two files that include a Box3D
# header today.** scripts/check-box3d-containment.sh already fails the build
# when one appears outside src/physics/, so the directory is a boundary the
# project enforces rather than a list somebody has to keep. A new file in here
# is covered without anybody remembering.
#
# It is not PUBLIC on the box3d target, because that would push it onto every
# translation unit of engine_core and change float code generation for the
# renderer and the math library as well.
#
# Upstream tests the compiler rather than the platform, and so does this. MSVC
# is left alone for the same reason it is in third_party/CMakeLists.txt.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${engine_physics_sources}
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off"
    )
endif()

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
    target_link_libraries(engine_core PRIVATE miniaudio::miniaudio engine_miniaudio)
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
