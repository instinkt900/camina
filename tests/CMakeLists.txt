# M0 keeps the test setup minimal. A test framework arrives when there is
# something that needs one. See rule 4.6 in DESIGN.md.

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE engine::core)
engine_set_warnings(test_core)
engine_enable_clang_tidy(test_core)

add_test(NAME core COMMAND test_core)

add_executable(test_reflect test_reflect.cpp)
target_link_libraries(test_reflect PRIVATE engine::core)
engine_set_warnings(test_reflect)
engine_enable_clang_tidy(test_reflect)

add_test(NAME reflect COMMAND test_reflect)

# Rule 4.1. No file outside src/render/vulkan/ can include a Vulkan header.
#
# The check is a shell script, so it needs bash. A Windows machine without Git
# Bash does not get the test. The vulkan-containment job in CI runs on Linux and
# covers every push, so the rule still holds.
find_program(BASH_EXE bash)
if(BASH_EXE)
    add_test(
        NAME vulkan_containment
        COMMAND ${CMAKE_COMMAND} -E env ${BASH_EXE}
                ${CMAKE_SOURCE_DIR}/scripts/check-vulkan-containment.sh
                ${CMAKE_SOURCE_DIR}
    )
else()
    message(STATUS "bash not found, skipping the vulkan_containment test")
endif()
