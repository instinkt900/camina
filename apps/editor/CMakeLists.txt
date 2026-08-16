add_executable(editor
    main.cpp
    ../screenshot.cpp
)

# stb_image_write carries its own implementation, and screenshot.cpp asks for it.
# The header is third-party code we do not patch, so the engine warning set and
# clang-tidy stay off that file. apps/runtime does the same for the same file.
set_source_files_properties(../screenshot.cpp PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)

# The same two libraries the runtime links, which is rule 4.3: one game, two
# applications. The editor opens its own ImGui windows, so it needs the ImGui
# headers as well as the engine.
# stb is for the screenshot encoder alone.
target_link_libraries(editor PRIVATE
    engine::core
    camina::sandbox
    SDL3::SDL3
    imgui::imgui
    stb::stb
)

# Rule 4.3 again. The macro removes editor-only code from every other target,
# so this is the one place it is defined. reflect::EditorOnly is the first
# consumer, in issue #305.
target_compile_definitions(editor PRIVATE ENGINE_WITH_EDITOR)

# Where the source content is, which the running program cannot work out. The
# World panel saves the scene a person edits, and that is this tree rather than
# the cooked one beside the executable. A build moved away from its source tree
# gets a directory that is not there, and the panel then disables its save
# button. apps/runtime does the same thing for hot reload.
target_compile_definitions(editor PRIVATE
    ENGINE_GAME_CONTENT_SOURCE="${CMAKE_SOURCE_DIR}/sandbox/content"
)

engine_set_warnings(editor)
engine_enable_clang_tidy(editor)
