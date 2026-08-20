# The moth_ui integration. This target exists only when with_ui is on.
#
# engine_core does not link it and never will. DESIGN.md section 8.5 keeps game
# UI an optional dependency, so that a build without it carries none of moth_ui
# and a replacement stays possible. The applications link this target instead.
add_library(engine_ui STATIC
    ui.cpp
    font.cpp
    font_factory.cpp
    image.cpp
    input_bridge.cpp
    layout_loader.cpp
    renderer.cpp
    stb_rect_pack_impl.cpp
    ui_pass.cpp
)

add_library(engine::ui ALIAS engine_ui)

# stb_rect_pack carries its own implementation, and this file asks for it. It
# holds nothing else, so the warning set and clang-tidy stay off it without
# covering any of our own code. SKIP_LINTING is a source file property, not a
# target property, so it has to sit here.
set_source_files_properties(stb_rect_pack_impl.cpp PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)

target_include_directories(engine_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(engine_ui
    PUBLIC
        engine::core
        # moth_ui types appear in this target's public headers, because the
        # interfaces it implements are moth_ui interfaces.
        moth_ui::moth_ui
    PRIVATE
        # FreeType rasterizes a glyph, HarfBuzz shapes a string, and stb packs
        # the atlas. All three stay inside font.cpp, so none of them reaches a
        # consumer of this target. See DESIGN.md section 8.3.
        Freetype::Freetype
        harfbuzz::harfbuzz
        stb::stb
)

engine_set_warnings(engine_ui)
engine_enable_clang_tidy(engine_ui)
