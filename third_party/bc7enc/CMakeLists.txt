# A thin wrapper over the bc7enc_rdo submodule.
#
# The submodule is a whole tool, with an ISPC kernel, a PNG reader, a DDS
# writer, and a rate-distortion pass. The cooker wants one thing out of it: the
# block encoder. So this target compiles bc7enc.cpp alone and exposes the
# directory that holds bc7enc.h.
#
# The rest of the submodule stays unbuilt. That keeps the build short and keeps
# lodepng and miniz out of our binaries, where stb and the cooker already cover
# the same ground.

set(bc7enc_root "${CMAKE_CURRENT_SOURCE_DIR}/../bc7enc_rdo")

if(NOT EXISTS "${bc7enc_root}/bc7enc.cpp")
    message(FATAL_ERROR
        "third_party/bc7enc_rdo is empty. Run:\n"
        "  git submodule update --init --recursive")
endif()

add_library(bc7enc STATIC "${bc7enc_root}/bc7enc.cpp")

target_include_directories(bc7enc SYSTEM PUBLIC "${bc7enc_root}")

# This is third-party code we do not patch, so the engine warning set and
# clang-tidy stay off it. Both would report problems in code we will never
# change. vk_vma.cpp does the same, for the same reason.
#
# SKIP_LINTING is a source file property, not a target property, so it goes on
# the file. Setting it on the target does nothing and reports nothing.
set_source_files_properties("${bc7enc_root}/bc7enc.cpp" PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)
