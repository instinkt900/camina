# Runs clang-tidy as part of the compile step. ENGINE_ENABLE_CLANG_TIDY decides.
# A Debug build or a CI build turns it on by default, and a RelWithDebInfo build
# outside CI leaves it off to stay fast while iterating. MSVC is the exception
# and defaults off whatever the build type, because clang-tidy reads a clang
# command line and the MSVC driver passes flags it does not understand. The Linux
# clang job covers every file, so nothing escapes review.
#
# The default is read once, on the first configure of a build directory. After
# that the cache holds the answer. See the block above the option below for why
# that matters more than it sounds.
#
# CI has no excuse for a missing clang-tidy, so a missing binary fails the build
# there instead of skipping the check without saying so.

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

# Reading the environment on every configure is what made this fragile. CMake
# does not re-run only when a person asks it to. Ninja re-runs it on its own, and
# that reconfigure inherits the environment of whoever started the build.
# tools/cooker/CMakeLists.txt globs the content trees with CONFIGURE_DEPENDS, so
# one new file under sandbox/content/ triggers it, and running the runtime writes
# such a file on its own.
#
# So a person who configured with CI=1 and then ran a plain ninja lost the check
# for every target, and the build passed having linted nothing. That is the same
# fail-open shape as the wrong clang-tidy above, and it cost more, because it hit
# the person following CLAUDE.md to predict CI before a push. Issue #287.
#
# The cache is the fix. The environment and the build type choose the default on
# the first configure only, and the answer then lives in the build directory,
# where a reconfigure with no environment cannot change it. option() writes the
# cache only when the variable is not already set, which is exactly that rule.
if(MSVC)
    # clang-tidy reads a clang command line, and the MSVC driver passes flags it
    # does not understand. The Linux clang job covers every file, so nothing
    # escapes review.
    set(engine_clang_tidy_default OFF)
elseif(DEFINED ENV{CI} OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(engine_clang_tidy_default ON)
else()
    set(engine_clang_tidy_default OFF)
endif()

option(ENGINE_ENABLE_CLANG_TIDY
       "Run clang-tidy in the compile step"
       ${engine_clang_tidy_default})

# Refusing here beats accepting the flag and linting nothing. Reachable only when
# somebody sets the option by hand, because the default above is off on MSVC.
if(ENGINE_ENABLE_CLANG_TIDY AND MSVC)
    message(FATAL_ERROR
            "ENGINE_ENABLE_CLANG_TIDY is ON, but clang-tidy cannot read an MSVC "
            "command line. Configure with -DENGINE_ENABLE_CLANG_TIDY=OFF.")
endif()

# Resolve the binary here rather than only in the function, so the report below
# can tell what was asked for from what will actually run. "On" printed over a
# build that lints nothing is the failure this whole file exists to prevent.
#
# A missing binary does not fail here. The function does that, and only for a
# target that wants linting, so a docs-only configure still works with clang-tidy
# absent. The docs job in .github/workflows/ci.yml is exactly that: it installs
# Doxygen alone, and CI is set in its environment.
#
# The versioned name first. Unversioned resolves to clang-tidy 18 on a stock
# Ubuntu noble and on the CI runner, which already carries an alternative that
# update-alternatives does not outrank.
#
# The variable is named for the engine rather than called CLANG_TIDY_EXE, which
# is what it used to be. find_program keeps a cache entry that already holds a
# path and ignores NAMES entirely, so a build directory configured before that
# change would hold on to the clang-tidy 18 it found then. A new name cannot
# collide with that. Override it with -DENGINE_CLANG_TIDY_EXE=<path>.
if(ENGINE_ENABLE_CLANG_TIDY)
    find_program(ENGINE_CLANG_TIDY_EXE NAMES clang-tidy-19 clang-tidy)
endif()

# Say what will happen, once for each configure. A gate that is off has to be
# visible, because a build that checked nothing looks exactly like a build that
# checked everything.
if(NOT ENGINE_ENABLE_CLANG_TIDY)
    if(MSVC)
        message(STATUS "clang-tidy: off, because clang-tidy cannot read an MSVC command line")
    else()
        message(STATUS
                "clang-tidy: off, so nothing lints. "
                "Configure with -DENGINE_ENABLE_CLANG_TIDY=ON to turn it on")
    endif()
elseif(NOT ENGINE_CLANG_TIDY_EXE)
    message(STATUS
            "clang-tidy: off, because no clang-tidy binary was found. "
            "Install clang-tidy-19, or set -DENGINE_CLANG_TIDY_EXE=<path>")
else()
    message(STATUS "clang-tidy: on, ${ENGINE_CLANG_TIDY_EXE}")
endif()

function(engine_enable_clang_tidy target)
    if(NOT ENGINE_ENABLE_CLANG_TIDY)
        return()
    endif()

    if(NOT ENGINE_CLANG_TIDY_EXE)
        # CI has no excuse for a missing clang-tidy. This lives here and not
        # beside the resolve above, so that a configure which defines no target
        # never reaches it. The report above has already said the gate is off.
        if(DEFINED ENV{CI})
            message(FATAL_ERROR "clang-tidy not found but required in CI")
        endif()
        return()
    endif()

    engine_verify_clang_tidy_config("${ENGINE_CLANG_TIDY_EXE}")
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${ENGINE_CLANG_TIDY_EXE}")
    message(STATUS "clang-tidy enabled for ${target}")
endfunction()
