add_executable(runtime main.cpp)

target_link_libraries(runtime PRIVATE engine::core SDL3::SDL3)

engine_set_warnings(runtime)
engine_enable_clang_tidy(runtime)
