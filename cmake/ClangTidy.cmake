# Runs clang-tidy as part of the compile step, but only in a Debug build or in CI.
# A RelWithDebInfo build stays fast, which is what you want while iterating.
#
# CI has no excuse for a missing clang-tidy, so a missing binary fails the build
# there instead of skipping the check without saying so.
#
# MSVC is the exception. clang-tidy reads a clang command line, and the MSVC
# driver passes flags it does not understand. The Linux clang job enforces the
# check for every file, so nothing escapes review.

function(engine_enable_clang_tidy target)
    if(MSVC)
        return()
    endif()

    if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" OR DEFINED ENV{CI}))
        return()
    endif()

    # Unversioned, which is what CI resolves today. Preferring clang-tidy-19
    # here is the right answer and it is not taken yet, because switching turns
    # the gate on and two functions are over the complexity threshold behind it.
    # See issue #242.
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
