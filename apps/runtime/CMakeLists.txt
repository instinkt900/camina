add_executable(runtime
    main.cpp
    screenshot.cpp
)

# stb_image_write carries its own implementation, and screenshot.cpp asks for it.
# The header is third-party code we do not patch, so the engine warning set and
# clang-tidy stay off that file. tools/cooker does the same for stb_image.
set_source_files_properties(screenshot.cpp PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)

# The runtime opens its own ImGui windows around reflect::inspect(), so it needs
# the ImGui headers as well as the engine.
# camina::sandbox is the game module. The editor links the same library at M8,
# which is what rule 4.3 asks for: one game, two applications.
# stb is for the screenshot encoder alone.
target_link_libraries(runtime PRIVATE
    engine::core
    camina::sandbox
    SDL3::SDL3
    imgui::imgui
    stb::stb
)

engine_set_warnings(runtime)
engine_enable_clang_tidy(runtime)
