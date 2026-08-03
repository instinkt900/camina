add_executable(runtime main.cpp)

# The runtime opens its own ImGui windows around reflect::inspect(), so it needs
# the ImGui headers as well as the engine.
# camina::sandbox is the game module. The editor links the same library at M8,
# which is what rule 4.3 asks for: one game, two applications.
target_link_libraries(runtime PRIVATE engine::core camina::sandbox SDL3::SDL3 imgui::imgui)

engine_set_warnings(runtime)
engine_enable_clang_tidy(runtime)

# The shaders and the game assets, cooked next to this executable. The engine
# tree holds the shaders that render/ reads. The game tree holds the scene and
# the prefabs that sandbox/ reads.
engine_cook_content(runtime engine "${CMAKE_SOURCE_DIR}/src/render/shaders")
engine_cook_content(runtime game "${CMAKE_SOURCE_DIR}/sandbox/content")
