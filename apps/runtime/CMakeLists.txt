add_executable(runtime main.cpp)

# The runtime opens its own ImGui windows around reflect::inspect(), so it needs
# the ImGui headers as well as the engine.
# camina::sandbox is the game module. The editor links the same library at M8,
# which is what rule 4.3 asks for: one game, two applications.
target_link_libraries(runtime PRIVATE engine::core camina::sandbox SDL3::SDL3 imgui::imgui)

engine_set_warnings(runtime)
engine_enable_clang_tidy(runtime)
