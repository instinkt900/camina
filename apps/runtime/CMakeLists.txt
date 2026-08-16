add_executable(runtime
    main.cpp
    ../screenshot.cpp
)

# stb_image_write carries its own implementation, and screenshot.cpp asks for it.
# The header is third-party code we do not patch, so the engine warning set and
# clang-tidy stay off that file. tools/cooker does the same for stb_image.
set_source_files_properties(../screenshot.cpp PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)

# The runtime opens its own ImGui windows around reflect::inspect(), so it needs
# the ImGui headers as well as the engine.
# camina::sandbox is the game module. The editor links the same library at M9,
# which is what rule 4.3 asks for: one game, two applications.
# stb is for the screenshot encoder alone.
target_link_libraries(runtime PRIVATE
    engine::core
    camina::sandbox
    SDL3::SDL3
    imgui::imgui
    stb::stb
)

# M6. Game UI is optional, so the application links it rather than engine_core.
# ENGINE_WITH_UI is what main.cpp compiles the UI path against. Hard rule 4.3
# says the macro removes code and never changes it.
if(ENGINE_WITH_UI)
    target_link_libraries(runtime PRIVATE engine::ui)
    target_compile_definitions(runtime PRIVATE ENGINE_WITH_UI)
endif()

# M4.5. Hot reload runs the cooker over the source content tree, and neither
# path can be worked out from the running program. The cooker sits beside the
# executable, so that one is found at runtime. These two cannot be.
# A build away from its source tree gets a directory that is not there. Hot
# reload then reports that and turns itself off, which is what a shipped build
# should do. --watch overrides it.
target_compile_definitions(runtime PRIVATE
    ENGINE_GAME_CONTENT_SOURCE="${CMAKE_SOURCE_DIR}/sandbox/content"
    ENGINE_ENGINE_CONTENT_SOURCE="${CMAKE_SOURCE_DIR}/src/render/content"
)

engine_set_warnings(runtime)
engine_enable_clang_tidy(runtime)
