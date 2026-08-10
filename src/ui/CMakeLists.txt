# The moth_ui integration. This target exists only when with_ui is on.
#
# engine_core does not link it and never will. DESIGN.md section 8.5 keeps game
# UI an optional dependency, so that a build without it carries none of moth_ui
# and a replacement stays possible. The applications link this target instead.
add_library(engine_ui STATIC
    ui.cpp
)

add_library(engine::ui ALIAS engine_ui)

target_include_directories(engine_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(engine_ui
    PUBLIC
        engine::core
        # moth_ui types appear in this target's public headers, because the
        # interfaces it implements are moth_ui interfaces.
        moth_ui::moth_ui
)

engine_set_warnings(engine_ui)
engine_enable_clang_tidy(engine_ui)
