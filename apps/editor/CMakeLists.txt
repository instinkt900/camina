add_executable(editor
    main.cpp
)

# The same two libraries the runtime links, which is rule 4.3: one game, two
# applications. The editor opens its own ImGui windows, so it needs the ImGui
# headers as well as the engine.
target_link_libraries(editor PRIVATE
    engine::core
    camina::sandbox
    SDL3::SDL3
    imgui::imgui
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
