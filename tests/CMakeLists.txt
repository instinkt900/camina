# M0 keeps the test setup minimal. A test framework arrives when there is
# something that needs one. See rule 4.6 in DESIGN.md.

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE engine::core)
engine_set_warnings(test_core)
engine_enable_clang_tidy(test_core)

add_test(NAME core COMMAND test_core)

# Rule 4.1. No file outside src/render/vulkan/ can include a Vulkan header.
add_test(
    NAME vulkan_containment
    COMMAND ${CMAKE_COMMAND} -E env bash
            ${CMAKE_SOURCE_DIR}/scripts/check-vulkan-containment.sh
            ${CMAKE_SOURCE_DIR}
)
