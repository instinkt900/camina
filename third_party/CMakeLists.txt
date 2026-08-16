# Vendored dependencies. Rule 4.4 in DESIGN.md allows a submodule here only when
# Conan Center does not carry the package, or when we may need to patch it.
# Everything else comes from Conan.

add_subdirectory(bc7enc)

# Box3D. DESIGN.md section 5 gives the reasons it is a submodule at a pinned
# commit rather than a Conan package. It is an alpha, we will read and patch it
# more than any other dependency, and its C API removes every ABI concern.
#
# bc7enc has a wrapper directory beside its submodule, and this has none. That
# is because bc7enc_rdo is a whole tool that ships no library target we want,
# so the wrapper builds one. Box3D already builds exactly one library and calls
# it box3d, so a wrapper would only rename it.
#
# What this file does skip is the Box3D root CMakeLists, which is written for a
# top-level build. It sets the static MSVC runtime, profiles/windows-msvc asks
# Conan for the dynamic one, and MSVC does not link objects that disagree about
# the runtime. It also turns on verbose makefiles and pulls in FetchContent.
# src/CMakeLists.txt under it builds the library and nothing else.
set(box3d_root "${CMAKE_CURRENT_SOURCE_DIR}/box3d")

if(NOT EXISTS "${box3d_root}/src/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/box3d is empty. Run:\n"
        "  git submodule update --init --recursive")
endif()

add_subdirectory(box3d/src)

# The Box3D root CMakeLists turns contraction into a fused multiply-add off, for
# the determinism its solver depends on. Skipping that file skips the flag, so
# set it here on the library. Upstream tests the compiler rather than the
# platform, and so does this.
#
# It reaches the library alone. Engine code that calls the inline math in the
# Box3D headers compiles it with contraction on, which is issue #229.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(box3d PRIVATE -ffp-contract=off)
endif()

# The Box3D CMakeLists declares its include directory PUBLIC rather than SYSTEM,
# so the engine warning set applies to Box3D headers that reach engine code.
# math_functions.h alone is 88 -Wold-style-cast warnings in the one file under
# src/physics/ that includes it. Mark the interface SYSTEM, the way the bc7enc
# wrapper does, so the compiler stays quiet about code we did not write.
get_target_property(box3d_includes box3d INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(box3d PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${box3d_includes}"
)

# Third-party code, so the warning set and clang-tidy stay off it, the same way
# bc7enc.cpp and vk_vma.cpp carry it. Both report problems in code we did not
# write, and a Box3D update would then arrive as a wall of them.
#
# This covers the sources. The headers reach us through src/physics/, which is
# our own translation unit, so ExcludeHeaderFilterRegex in .clang-tidy is what
# keeps them out.
#
# SKIP_LINTING is a source file property, not a target property, and a source
# file property belongs to the directory that declared the file. So this needs
# the DIRECTORY form to reach sources the Box3D CMakeLists added.
get_target_property(box3d_sources box3d SOURCES)
list(TRANSFORM box3d_sources PREPEND "${box3d_root}/src/")

set_source_files_properties(${box3d_sources}
    DIRECTORY box3d/src
    PROPERTIES
        SKIP_LINTING ON
        COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>"
)

# ImGuizmo. The gizmo the editor puts on the selected entity, in M9.5.
#
# Behind ENGINE_WITH_EDITOR, because the editor application is the only thing
# that draws one. Rule 4.3 says the editor is an application rather than a build
# mode, and this is the option that gates that application. A runtime build
# compiles none of it.
#
# It is a submodule rather than a Conan package because no usable package
# exists. Both recipes on Conan Center, imguizmo/1.83 and
# imguizmo/cci.20231114, require imgui/1.90.5, and this engine is on the
# docking branch of 1.92. Conan then refuses the whole graph, which is what
# stopped with_editor=True from configuring at all. See issue #308 and
# DESIGN.md section 5.
#
# One source file of the several the repository ships. The sequencer, the
# curve editor, the gradient widget, and the graph editor are separate tools
# that the editor does not use, and each one is more code to compile and to
# keep compiling.
if(ENGINE_WITH_EDITOR)
    set(imguizmo_root "${CMAKE_CURRENT_SOURCE_DIR}/imguizmo")

    if(NOT EXISTS "${imguizmo_root}/src/ImGuizmo.cpp")
        message(FATAL_ERROR
            "third_party/imguizmo is empty. Run:\n"
            "  git submodule update --init --recursive")
    endif()

    add_library(imguizmo STATIC "${imguizmo_root}/src/ImGuizmo.cpp")

    target_include_directories(imguizmo SYSTEM PUBLIC "${imguizmo_root}/src")

    # PUBLIC, because ImGuizmo.h includes imgui.h, so every consumer needs the
    # same headers this was built against.
    target_link_libraries(imguizmo PUBLIC imgui::imgui)

    # Third-party code, so the engine warning set and clang-tidy stay off it,
    # the same as bc7enc and the VMA implementation file. SKIP_LINTING is a
    # source file property rather than a target property.
    set_source_files_properties("${imguizmo_root}/src/ImGuizmo.cpp" PROPERTIES
        SKIP_LINTING ON
        COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
    )
endif()
