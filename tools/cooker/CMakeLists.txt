# The asset cooker. DESIGN.md section 6 makes it a separate executable, so a
# cook runs on a build machine with no graphics driver.
#
# The logic is a library and main.cpp is only the command line, so a test can
# cook a tree and check what came out.
add_library(cooker_lib STATIC
    cook.cpp
)

target_include_directories(cooker_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# engine::core brings the GUID, the sidecar, and the manifest. It brings no
# Vulkan into this target, because engine_core keeps volk PRIVATE.
target_link_libraries(cooker_lib PUBLIC engine::core)

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

# Cooks one source tree next to a target's executable, and makes it a
# dependency of that target.
#
# The output lands beside the executable rather than in a fixed build path,
# because platform::cooked_content_root() finds it that way at run time. So a
# runtime started from a shell, a debugger, or a file manager all read the same
# directory.
#
# target:  the executable that needs the cooked content at run time.
# name:    the subdirectory under the cooked root, so two trees stay apart.
# source:  the source content directory.
function(engine_cook_content target name source)
    set(out_dir "$<TARGET_FILE_DIR:${target}>/content/${name}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${name}.cooked")

    # The stamp exists because the cooker writes a directory rather than one
    # file, and a custom command needs an output CMake can compare. The cooker
    # skips what it already built, so a rerun is cheap.
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

    add_custom_target(${target}_content_${name} DEPENDS "${stamp}")
    add_dependencies(${target} ${target}_content_${name})
endfunction()
