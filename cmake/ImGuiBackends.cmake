# The ImGui backends, taken from the Conan package.
#
# ImGui ships the platform and renderer backends as loose source files, because
# each application compiles only the pair it uses. The Conan package copies them
# to res/bindings, and the std::string helper to res/misc/cpp, but it does not
# declare a resource directory. CMakeDeps therefore leaves imgui_RES_DIRS empty,
# and we work from the package root instead.
#
# Rule 4.4 says we vendor only what we patch, and we patch none of this, so the
# files stay in the Conan cache and we compile them where they are. They are
# third-party, so they build with warnings and clang-tidy off, the same as VMA.

# CMakeDeps names the package folder after the configuration. Ninja builds one
# configuration at a time, so the first name that answers is the right one.
function(_engine_imgui_package_root out_var)
    foreach(config RELWITHDEBINFO RELEASE DEBUG MINSIZEREL)
        if(imgui_PACKAGE_FOLDER_${config})
            set(${out_var} "${imgui_PACKAGE_FOLDER_${config}}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# Adds the SDL3 platform backend, the Vulkan renderer backend, and the
# std::string helper to a target.
#
# The target must already link imgui::imgui, SDL3::SDL3, and volk::volk.
function(engine_add_imgui_backends target)
    _engine_imgui_package_root(root)
    if(NOT root)
        message(FATAL_ERROR
            "The imgui package folder was not found. Run conan install first, "
            "and check that find_package(imgui) came before this call.")
    endif()

    set(bindings "${root}/res/bindings")
    set(misc "${root}/res/misc/cpp")

    set(sources
        "${bindings}/imgui_impl_sdl3.cpp"
        "${bindings}/imgui_impl_vulkan.cpp"
        "${misc}/imgui_stdlib.cpp"
    )

    foreach(source ${sources})
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR
                "${source} is missing. The imgui Conan package changed its "
                "layout. Update cmake/ImGuiBackends.cmake.")
        endif()
    endforeach()

    target_sources(${target} PRIVATE ${sources})

    set_source_files_properties(${sources} PROPERTIES
        SKIP_LINTING ON
        COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
    )

    # res holds misc/cpp/imgui_stdlib.h, and bindings holds the two backend
    # headers. Both are private, because no public header names them.
    target_include_directories(${target} PRIVATE "${root}/res" "${bindings}")

    # The engine loads Vulkan through volk and never links the loader library,
    # so the backend must use the volk function pointers as well. Without this
    # it takes the prototypes from vulkan.h and the link step fails.
    target_compile_definitions(${target} PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK)
endfunction()
