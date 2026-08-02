add_executable(runtime main.cpp)

# The runtime opens its own ImGui windows around reflect::inspect(), so it needs
# the ImGui headers as well as the engine.
target_link_libraries(runtime PRIVATE engine::core SDL3::SDL3 imgui::imgui)

engine_set_warnings(runtime)
engine_enable_clang_tidy(runtime)
