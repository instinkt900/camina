# Runs clang-tidy as part of the compile step, but only in a Debug build or in CI.
# A RelWithDebInfo build stays fast, which is what you want while iterating.
#
# CI has no excuse for a missing clang-tidy, so a missing binary fails the build
# there instead of skipping the check without saying so.
#
# MSVC is the exception. clang-tidy reads a clang command line, and the MSVC
# driver passes flags it does not understand. The Linux clang job enforces the
# check for every file, so nothing escapes review.

# The gate is worth only what clang-tidy actually reads, and a version that
# rejects one key in .clang-tidy throws the whole file away. It then lints with
# its own defaults, which carry no Checks list and no WarningsAsErrors. It says
# so on stderr and carries on, so the build passes and the gate checked almost
# nothing. ExcludeHeaderFilterRegex needs clang-tidy 19, and 18 rejects it.
# That is issue #242.
#
# So this refuses to configure rather than lint nothing.
#
# --config-file is what makes it a real test. A bare --verify-config finds no
# .clang-tidy at all. It prints any parse error it hits, then reports "No config
# errors detected" and exits 0 anyway, from any directory.
function(engine_verify_clang_tidy_config tidy_exe)
    # Once for each configure, not once for each target. A global property
    # clears when CMake runs again, so an edit to either file is rechecked.
    get_property(checked GLOBAL PROPERTY ENGINE_CLANG_TIDY_CONFIG_CHECKED)
    if(checked)
        return()
    endif()

    set(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    foreach(config "${root}/.clang-tidy" "${root}/tests/.clang-tidy")
        execute_process(
            COMMAND "${tidy_exe}" --verify-config "--config-file=${config}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output)
        if(NOT result EQUAL 0)
            get_filename_component(shown "${config}" ABSOLUTE)
            message(FATAL_ERROR
                    "${tidy_exe} rejected ${shown}, so it would check almost nothing. "
                    "This needs clang-tidy 19 or later. See issue #242.\n${output}")
        endif()
    endforeach()

    set_property(GLOBAL PROPERTY ENGINE_CLANG_TIDY_CONFIG_CHECKED ON)
endfunction()

function(engine_enable_clang_tidy target)
    if(MSVC)
        return()
    endif()

    if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" OR DEFINED ENV{CI}))
        return()
    endif()

    # The versioned name first. Unversioned resolves to clang-tidy 18 on a stock
    # Ubuntu noble and on the CI runner, which already carries an alternative
    # that update-alternatives does not outrank.
    #
    # The variable is named for the engine rather than called CLANG_TIDY_EXE,
    # which is what it used to be. find_program keeps a cache entry that already
    # holds a path and ignores NAMES entirely, so a build directory configured
    # before this change would hold on to the clang-tidy 18 it found then. A new
    # name cannot collide with that. Override it with
    # -DENGINE_CLANG_TIDY_EXE=<path>.
    find_program(ENGINE_CLANG_TIDY_EXE NAMES clang-tidy-19 clang-tidy)
    if(NOT ENGINE_CLANG_TIDY_EXE)
        if(DEFINED ENV{CI})
            message(FATAL_ERROR "clang-tidy not found but required in CI")
        endif()
        message(STATUS "clang-tidy not found, skipping for ${target}")
        return()
    endif()

    engine_verify_clang_tidy_config("${ENGINE_CLANG_TIDY_EXE}")
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${ENGINE_CLANG_TIDY_EXE}")
    message(STATUS "clang-tidy enabled for ${target}")
endfunction()
