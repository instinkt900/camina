# Compiles GLSL to SPIR-V at build time and writes a C initializer list.
#
# The engine embeds the result in the binary, so the runtime carries no shader
# compiler. glslc comes from the shaderc tool_requires in conanfile.py, which
# puts it on PATH through the Conan build environment.
#
# M4 moves shader cooking into tools/cooker/. Until then this rule covers the
# few shaders the engine builds into itself.

find_program(ENGINE_GLSLC_EXE NAMES glslc)
if(NOT ENGINE_GLSLC_EXE)
    message(FATAL_ERROR
        "glslc was not found. Run conan install first, and make sure the "
        "generated build environment is active.")
endif()
message(STATUS "Using glslc at ${ENGINE_GLSLC_EXE}")

# Compiles each GLSL file and attaches the output to a target.
#
# Each shader becomes <binary_dir>/shaders/<name>.spv.inc, holding a braced list
# of 32-bit words. Include it inside std::to_array to embed the module.
#
# target: the target that consumes the shaders.
# ARGN:   GLSL files, relative to the current source directory.
function(engine_add_shaders target)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${out_dir}")

    set(outputs "")
    foreach(shader IN LISTS ARGN)
        get_filename_component(name "${shader}" NAME)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${shader}")
        set(output "${out_dir}/${name}.spv.inc")

        # -mfmt=c writes the words as a braced C initializer list.
        # --target-env matches the Vulkan version the device asks for.
        # Shaders have no #include yet, so this needs no depfile. Add one here
        # when a shared shader header appears.
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${ENGINE_GLSLC_EXE}"
                    --target-env=vulkan1.3
                    -O
                    -mfmt=c
                    -o "${output}"
                    "${source}"
            DEPENDS "${source}"
            COMMENT "Compiling shader ${name}"
            VERBATIM
        )
        list(APPEND outputs "${output}")
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
