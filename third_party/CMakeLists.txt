# Vendored dependencies. Rule 4.4 in DESIGN.md allows a submodule here only when
# Conan Center does not carry the package, or when we may need to patch it.
# Everything else comes from Conan.

add_subdirectory(bc7enc)

# Box3D. DESIGN.md section 5 gives the reasons it is a submodule at a pinned
# commit rather than a Conan package: it is an alpha, we will read and patch it
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
