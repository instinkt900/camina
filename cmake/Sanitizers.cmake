# AddressSanitizer and UndefinedBehaviorSanitizer, applied to the whole build.
# ENGINE_SANITIZERS decides. It is empty by default, so a normal build carries
# no sanitizer and pays nothing.
#
# This exists because of issue #450. A heap use-after-free in the mixer went
# unfound for months. Windows CI failed about one run in six and the cause read
# as a flaky runner. AddressSanitizer named it in one run, with the allocation,
# the free and the read all pointing at three lines of one function.
#
# The flags go on every target rather than on engine code alone. A sanitizer is
# a run-time check, so instrumenting only half the program means the other half
# reports nothing. That covers the vendored sources in third_party/ as well,
# which carry -w for the compiler warnings and still get the run-time check.
#
# Set it from the configure line:
#
#     cmake --preset conan-relwithdebinfo -DENGINE_SANITIZERS=address,undefined
#
# Do not confuse this with the poisoned Conan cache in CLAUDE.md. That section
# describes Debug packages built elsewhere on the machine with a sanitizer,
# which Conan then reuses for this project. This adds the flags to this project
# only and leaves the cache alone.

set(ENGINE_SANITIZERS "" CACHE STRING
    "Sanitizers to build with, comma separated. For example: address,undefined")

if(NOT ENGINE_SANITIZERS)
    message(STATUS "sanitizers: off")
    return()
endif()

# MSVC has an AddressSanitizer of its own, under a different flag, with a
# runtime DLL to ship and no UndefinedBehaviorSanitizer at all. Refusing beats
# passing a flag the driver ignores, which would report a clean build that
# checked nothing.
if(MSVC)
    message(FATAL_ERROR
            "ENGINE_SANITIZERS is set, and this module builds Clang and GCC "
            "flags that the MSVC driver does not take. Configure with "
            "-DENGINE_SANITIZERS= on MSVC.")
endif()

# -fno-omit-frame-pointer is what makes a report name the function rather than
# an address. -g gives it the line.
set(engine_sanitizer_flags
    "-fsanitize=${ENGINE_SANITIZERS}"
    -fno-omit-frame-pointer
    -g)

# **UndefinedBehaviorSanitizer prints and carries on by default.** So a signed
# overflow reaches the log, the test still passes, and the gate reports green
# over the bug it found. -fno-sanitize-recover turns every finding into an
# abort, which is what ctest can see. AddressSanitizer already aborts.
if(ENGINE_SANITIZERS MATCHES "undefined")
    list(APPEND engine_sanitizer_flags -fno-sanitize-recover=all)
endif()

add_compile_options(${engine_sanitizer_flags})
add_link_options("-fsanitize=${ENGINE_SANITIZERS}")

message(STATUS "sanitizers: on, ${ENGINE_SANITIZERS}")
