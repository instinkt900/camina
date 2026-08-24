# The asset cooker. DESIGN.md section 6 makes it a separate executable, so a
# cook runs on a build machine with no graphics driver.
#
# The rules moved to src/import/ at M13.2, because the editor runs them too.
# What is left here is the command line and the content targets.
#
# engine::import knows no game. The executable picks one, the same way
# apps/runtime does, because a document may carry a component the game defines
# and only the descriptors say which of its fields name an asset.
# camina::sandbox links engine::core and nothing else, so this brings in no
# driver and no window.
add_executable(cooker main.cpp)
target_link_libraries(cooker PRIVATE engine::import camina::sandbox)

# Measures what importing each asset type costs. DESIGN.md section 10 M13 holds
# the numbers, and this is how to take them again after a change to a rule.
add_executable(measure_import measure.cpp)
target_link_libraries(measure_import PRIVATE engine::import camina::sandbox)
engine_set_warnings(measure_import)
engine_enable_clang_tidy(measure_import)

engine_set_warnings(cooker)
engine_enable_clang_tidy(cooker)

# Cooks one source tree into the directory the executables run from.
#
# The output goes beside the executables rather than under a build path,
# because platform::cooked_content_root() looks next to the running program.
# One cook therefore serves the runtime, the editor, and the tests.
#
# The stamp lives inside the cooked directory rather than in the build tree, so
# the two cannot drift. Deleting the cooked content deletes the stamp with it,
# and the next build cooks again. A stamp left behind in the build tree would
# stay newer than every input, the build would skip the cooker, and a program
# would start with nothing to draw.
#
# name:   the subdirectory under the cooked root, so two trees stay apart.
# source: the source content directory.
function(engine_cook_content name source)
    set(out_dir "${ENGINE_COOKED_CONTENT_DIR}/${name}")
    set(stamp "${out_dir}/.cooked")

    file(GLOB_RECURSE cook_inputs CONFIGURE_DEPENDS "${source}/*")

    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND cooker
                --content "${source}"
                --out "${out_dir}"
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
        DEPENDS cooker ${cook_inputs}
        COMMENT "Cooking ${name} content"
        VERBATIM
    )

    add_custom_target(content_${name} ALL DEPENDS "${stamp}")
endfunction()

# The engine tree holds the shaders and the textures that render/ reads. The
# game tree holds the scene and the prefabs that sandbox/ reads.
engine_cook_content(engine "${CMAKE_SOURCE_DIR}/src/render/content")
engine_cook_content(game "${CMAKE_SOURCE_DIR}/sandbox/content")

# The test tree holds one scene of opaque, single sided geometry, which the GPU
# frame check renders. The sandbox cannot be that scene, because its glass panes
# are blended and double sided. See tests/content/README.md and issue #190.
engine_cook_content(test "${CMAKE_SOURCE_DIR}/tests/content")

# The sky tree holds one blended pane over open sky, which is the case issue
# #435 broke. The test tree cannot be that scene, because it names no
# environment on purpose, and the sandbox cannot either, because it is a closed
# room. See tests/content_sky/README.md.
engine_cook_content(sky "${CMAKE_SOURCE_DIR}/tests/content_sky")
