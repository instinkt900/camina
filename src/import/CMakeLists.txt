# The import rules: source assets in, cooked assets out. DESIGN.md section 10
# M13 makes this a library rather than code inside the cooker, because the
# editor runs the same rules the cooker runs and has to produce the same bytes.
#
# It links no Vulkan and opens no window. An import runs on a build machine with
# no graphics driver, and CI is exactly that machine.
add_library(engine_import STATIC
    cook.cpp
    rules.cpp
    source_assets.cpp
    writer.cpp
    document.cpp
    layout.cpp
    brdf.cpp
    environment.cpp
    material.cpp
    mesh.cpp
    prefab.cpp
    shader.cpp
    sound.cpp
    texture.cpp
    stb_image_impl.cpp
    to_half.h
)

# stb_image carries its own implementation, and this file asks for it. It holds
# nothing of ours, so the engine warning set and clang-tidy stay off it. Rule
# 4.4 keeps the file in the Conan cache rather than in third_party/, because we
# do not patch it.
set_source_files_properties(stb_image_impl.cpp PROPERTIES
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>"
)

add_library(engine::import ALIAS engine_import)

# engine::core brings the GUID, the sidecar, and the manifest, and its PUBLIC
# include directory is what makes "import/mesh.h" resolve. It brings no Vulkan
# into this target, because engine_core keeps volk PRIVATE.
#
# Every importer dependency is PRIVATE. That is what lets the editor link this
# target without growing stb, cgltf, meshoptimizer, bc7enc or shaderc in its
# own interface, and it is what keeps them out of a runtime build entirely.
target_link_libraries(engine_import
    PUBLIC
        engine::core
    PRIVATE
        stb::stb
        bc7enc
        cgltf::cgltf
        meshoptimizer::meshoptimizer
        spirv-reflect-static
        shaderc::shaderc
)

engine_set_warnings(engine_import)
engine_enable_clang_tidy(engine_import)
