# Runs clang-tidy as part of the compile step, but only in a Debug build or in CI.
# A RelWithDebInfo build stays fast, which is what you want while iterating.
#
# CI has no excuse for a missing clang-tidy, so a missing binary fails the build
# there instead of skipping the check without saying so.

function(engine_enable_clang_tidy target)
    if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" OR DEFINED ENV{CI}))
        return()
    endif()

    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
        message(STATUS "clang-tidy enabled for ${target}")
    elseif(DEFINED ENV{CI})
        message(FATAL_ERROR "clang-tidy not found but required in CI")
    else()
        message(STATUS "clang-tidy not found, skipping for ${target}")
    endif()
endfunction()
