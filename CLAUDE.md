# CLAUDE.md

Guidance for Claude Code in this repository.

## Project

**Camina Engine.** A small 3D game engine in C++20. Vulkan rendering, EnTT, Lua
scripting, and a separate editor application and runtime application over one shared
core.

**The C++ namespace is `engine`, not `camina`.** This is deliberate. The name appears
in the CMake project, the Conan package, release artifacts, the window title, and the
docs. It does not appear in any C++ identifier. Targets stay `engine_core` and
`engine::core`, macros stay `ENGINE_`. Do not rename them. See `DESIGN.md` §2.1.

**`DESIGN.md` is the source of truth for architecture.** Read it before you answer any
question about structure, dependencies, or sequencing. Do not repeat its content here.
Update it when a decision changes.

**Two platforms, one compiler each.** Linux uses Clang 19 or later, Windows uses MSVC
from Visual Studio 2022. GCC is not a target and there is no GCC profile. CI builds and
tests both platforms on every pull request, and a release carries an archive for each.

## Current status

M2 is complete. A type describes itself once in a `Describe<T>` specialization, and two
consumers read that one description. The ImGui inspector generates its widgets from the
descriptors, and the JSON serializer writes and reads the same fields. The runtime shows
both: it edits a `Scene` struct live and round-trips it to `scene.json`.

M1 is complete as well. The runtime draws a spinning textured cube through Vulkan, with a
swapchain that survives resize, two frames in flight, dynamic rendering, reverse-Z depth,
and clean validation. It arrived in three parts: the device and the swapchain, the shader
build and the triangle, then buffers, textures, descriptors, and the cube.

M3 is complete: EnTT and the scene. `scene::World` holds the registry and a transform
hierarchy that rebuilds a world matrix only when something moved (M3.1). A `.scene` file
reads and writes that world through the reflection descriptors (M3.2). A prefab is a scene
fragment, and an instance stores only the fields it overrides (M3.3). `sandbox/` is a
library that the runtime links, and it loads a scene of prefab instances and turns two of
them. The runtime flies through that scene and edits any component on any entity, which
closes M3. See `DESIGN.md` §10.

M4 is complete. M4.1 gives an asset its identity. It adds `engine::Guid` in
`src/core/guid.h`, a `.meta` sidecar next to each source file, and `assets::AssetDatabase`.
The database turns a GUID into a handle that stays valid when the asset loads or reloads.

M4.2 adds `tools/cooker/`. It walks a source tree, cooks what it has a rule for, and copies
what it does not. It writes a manifest of every output and the inputs it came from. A hash of the input bytes
decides what to skip, so a second cook of an unchanged tree does nothing. Shaders are the
first asset type on it, and `cmake/Shaders.cmake` is gone. Cooked content now sits next to
the executable, and `platform::cooked_content_root()` finds it there.

M4.4 came in three parts. M4.4a makes glTF the third asset type. The
cooker reads a `.gltf` or a `.glb` with cgltf, builds tangents when the source has none,
and reorders with meshoptimizer. One glTF holds several meshes, so the manifest now maps
one source to many outputs. A sub-asset has no sidecar, so `Guid::derive` works out its
identity from the parent GUID, a kind word, and an index. M4.4c draws it: `scene::MeshRenderer` names a mesh by GUID, `render::MeshPass` draws every
entity that has one, and `render::MeshCache` uploads each mesh once.
`runtime --screenshot <file>` writes the last frame as a PNG, which is the only way to check
that geometry is not mirrored or inside out.

M4.4b makes the material the fourth asset type. `src/assets/material.h` holds the cooked
format, which is one fixed-size header that names five textures by GUID. The importer writes
one material for each glTF material, a submesh names the one it uses, and an image sidecar is
now an input of the glTF file. `render::TextureCache` and `render::MaterialCache` turn those
GUIDs into handles, and `MeshPass` binds the base color of each submesh. The format carries
the whole metallic-roughness set and the renderer reads the base color. M5 reads the rest.

An image a glTF carries inside itself, in a buffer view or in a data URI, has no file and so
no sidecar. It gets a derived GUID under the kind word `texture`, and its color space comes
from the material slot that uses it rather than from a file name. That covers the `.glb`,
which is what most exporters produce.

M4.4 is complete. The importer turns the glTF node tree into a prefab, so a scene instances a
model rather than naming each of its cooked meshes by hand. The cooker adds a root when a
glTF scene lists several, because a prefab holds exactly one. `CubePass` is gone and every
entity draws through `MeshPass`, which needed the crates to become a real `crate.gltf` in the
game content tree.

The manifest now records which cooker wrote it. A rule that starts writing a new kind of
output changes nothing the freshness check looks at, so without this an old cooked tree stays
fresh forever and the new output never appears.

M4.3 makes a texture the second asset type. `src/assets/texture.h` holds the cooked format,
and both the cooker and the runtime read that one header. The cooker reads an image with
stb_image and builds the mip chain in linear light. It then compresses to BC7 with
`bc7enc_rdo`, the first entry in `third_party/`. The `.meta` sidecar records the color
space. The cooker guesses it from the file name, and only on the cook that writes a new
sidecar. The cube reads `cube.png` from the cooked tree, and `build_texture()` is gone.

A prefab instance records shape as well as fields, which closes a gap M3.3 left. The scene
format is version 3, and an instance record carries `removed`, `added`, and `reparented` next
to `overrides`. `scene::instantiate` takes the whole record now, not just the field patch.

M4.5 closes the milestone. `platform::DirectoryWatcher` polls the source tree and holds a
change back until the file stops moving. `platform::run_process` starts the cooker without a
shell. `assets::HotReload` joins them, and `Content::reload` compares the new manifest against
the old one so a save names one asset rather than the whole tree. `MeshPass::reload` waits for
the frames in flight before it frees anything. The runtime watches `sandbox/content`, and a
scene or a prefab that changes builds the world again. `--watch`, `--glslc`, and `--no-watch`
override it.

The runtime watches `src/render/content` as well, so a shader edit shows up live.
`MeshPass::reload_shaders` builds a new pipeline and keeps the old one when the new one will
not build. A cook that fails now keeps the manifest entry the last cook gave that asset,
because losing it hides a cooked file that is still on disk and stops the next start.

M5 is complete, and the paragraphs below record it in the order the parts landed rather than
by number. The close of the milestone is further down, under the Sponza scene.

The first half of M5.1 landed: a descriptor set layout now comes from the
cooked SPIR-V rather than from hand-written C++. The cooker links SPIRV-Reflect and writes
`src/assets/shader.h`, which carries the module, the bindings, and the members of every
uniform block in one file. `gfx::GraphicsPipelineDesc` takes those bindings, and
`sample_texture` is gone. The cook no longer passes `-O` to glslc, because spirv-opt strips
the names the parameter block needs. See issue #90.

The first half of M5.2 followed. `mesh.frag` is Cook-Torrance metallic-roughness now, and it
reads all five maps and every factor the cooked material carries. That needed a real
descriptor set in `gfx::`: `create_descriptor_set` takes what a reflected shader declared, and
`cmd_bind_texture` is gone. A texture no longer carries a set of its own, because a material
needs several textures and a block of factors bound together. `BufferUsage::Uniform` makes a
host-visible buffer that `update_buffer` writes. Set 0 is the frame and set 1 is the material,
and the frame block is one buffer for each frame in flight. The vertex tangent is declared at
last, which is what normal mapping needed.

M5.2 is complete. `scene::DirectionalLight` and `scene::PointLight` are reflected components,
so a person aims the sun by turning its entity and moves a lamp by moving one.

M5.7a lifted the eight-light ceiling those lights used to sit under. The list is a storage
buffer now, one for each frame in flight. The count is a number in the frame block rather than a
length the shader declares. The buffer doubles to fit rather than dropping what does not, so a
scene is never refused for carrying too many. `kMaxLights` is gone, which closed #98.

A point light whose range sphere misses the camera frustum never reaches the buffer.
`src/math/frustum.h` extracts the six planes and tests the sphere, and it names no Vulkan type,
so `tests/test_frustum.cpp` drives it with no GPU. Six mutations of it each fail a test.
Swapping two planes in the array fails nothing. That is correct rather than a gap, because every
reader iterates all six and the order carries no meaning.

The cull cuts the frame time to under a third when most lights are out of view. The measurement
is 11.2 ms against 36.0 ms, with 303 lights and the same geometry on screen. It is worth nothing
measurable in the sandbox, where all three lights are in view. Measuring it needed a scene built
for the purpose. One small room cannot hold many off-screen lights and a full view of the
geometry at once.

A first attempt measured the cull as slower. That run pointed the camera out of the open front,
where the frame costs about 1 ms and fixed overhead swamps the difference. A frame that is
mostly overhead cannot measure a change to the part that is not.

The descriptor pool has to reserve every type a set layout names. It reserved no storage buffer
at first, and this machine ran anyway, because that driver ignores the per-type counts. Another
vendor answers `VK_ERROR_OUT_OF_POOL_MEMORY` instead.

M5.7b closes the lighting work and closed #151. A compute pass divides the frustum into 16 tiles
across, 12 down, and 16 depth slices, and writes a short list of light indices for each cell.
`mesh.frag` works out which cell a fragment is in and loops over that list. Over a room of 1024
point lights with 837 in view, the mesh pass falls from 100.7 ms to 11.2 ms and the picture does
not move. The cull is 0.5 ms of that, and it is 0.009 ms in the sandbox.

It is the first compute path in the engine. `gfx::` gained `ComputePipelineDesc`,
`create_compute_pipeline`, `cmd_dispatch`, and `cmd_buffer_barrier`, and one pipeline handle and
one descriptor pool serve both kinds. The cull is a pass in the frame graph, so `kClusterGrid` is
the first graph resource that is a buffer rather than an image and the barrier falls out of
`derive_barriers`.

`BufferDesc::device_only` came from this. A uniform or a storage buffer is host-visible and
mapped by default, which is the wrong memory for three megabytes a shader fills and another
shader reads.

The slices are exponential in view distance. Linear in NDC depth looks reasonable and is not:
reverse-Z puts infinity at zero, so an even split of that range leaves fifteen of sixteen slices
inside the first 1.6 metres and the whole room in the last one. The picture stayed correct
throughout, because a cell that is too large only makes the cull conservative. That is why the
measurement was the thing that found it, and why a screenshot could not.

The last slice reaches past where the grid stops. A fragment beyond that distance clamps into
it, so a slice that ended there dropped a far light and left the surface unlit. Cutting the reach
to 4 metres and the light range to 3 moves 4.45 percent of the frame, which is how that one was
measured rather than argued.

A cell held 256 light indices. That number was measured. At 64 the 837-light scene lost light on
36 percent of the frame, by up to 228 of 255 on a channel. At 256 it matched a shader that loops
over every light byte for byte. So the grid was exact and the cap was the whole error. The drop
was still silent past 256, which was issue #175.

M5.7e made the capacity follow the light count, which closed #175. It doubles from 256 up to 2048
and the grid grows with it. Both shaders read the number out of a uniform they already bind. A
cell that holds every visible light cannot drop one, so the guarantee is structural. That is why
there is no drop counter. A drop needs a capacity below the visible count, and the frame report
says whether that holds.

The old cap was wrong long before 2048. Take 513 point lights of 8 m range. Capping a cell at 256
moves 44.6 percent of the frame against the fitted 1024, by up to 213 of 255. That costs what the
light costs. The mesh pass goes from 59.1 ms to 85.5 ms, because those lights are shaded rather
than dropped. The cull pays 0.07 ms of it. The sandbox does not move at all, and the pass
measures 1.452 ms on both sides.

Past the ceiling the light list is ordered by luminance times range. So a crowded cell keeps the
lights that put the most light into the scene. Deleting that sort moves the mean error of the
capped frame from 1.084 to 1.690 of 255. That is how it was measured rather than argued.

`--cluster-cell-lights` lowers the ceiling. No sandbox scene reaches the drop path now, so
measuring the loss needed a way to force it. The measurement scene is generated and not
committed, the way the 1024-light room was.

The alpha modes close it. Mask discards in the shader. Blend needs more than a shader, so
`MeshPass` holds a second pipeline that blends and does not write depth, and it gathers every
blended submesh, sorts it back to front, and draws it after the opaque ones.
`GraphicsPipelineDesc` gained `blend` and `depth_write` for that. `sandbox/content/models/glass/`
is two tinted panes, because nothing else in the sandbox is transparent and the path would
otherwise ship untested.

M5.4a replaced those two colors. The cooker turns an equirectangular `.hdr` into a cubemap,
`scene::Environment` names it by GUID, and `MeshPass` binds it at set 0 beside the frame block.
`mesh.frag` reads it twice, once along the normal at the smallest mip and once along the
reflection at a mip roughness chooses. That is not IBL, and issue #109 replaces the two samples
with the split sum form.

A `sampler2D` and a `samplerCube` need separate fallbacks, so `TextureCache` holds six grey
texels beside its white one and refuses a file whose face count does not match the binding.
Binding the wrong shape is undefined rather than an error, and the check is what keeps it out.

The cooker half of M5.4b followed. The `.hdr` rule writes two outputs now. The cubemap keeps the
source identity and its mip chain is GGX filtered by roughness rather than box filtered, so a
level is the environment blurred for one roughness. The second output is the irradiance, nine
RGB coefficients in `src/assets/irradiance.h`, under a GUID derived from the source under the
kind word `irradiance`. `specular_samples` in the sidecar sets the ray budget.

A constant sky only pins the first coefficient, because a uniform source has no higher band to
get wrong. So the tests also drive a band-limited source and check the coefficients against a
direct cosine-weighted integral. Corrupting any band constant fails that one, and corrupting
band 1 or 2 fails nothing without it.

The third part of the split sum is `tools/cooker/brdf.cpp`, keyed on the `.brdf` extension. It
integrates the table from `src/render/content/ibl.brdf`, a source file that carries no data
because the table depends on nothing. Its sidecar holds the size and the ray budget.

M5.4b is complete. `mesh.frag` reads all three parts by the split sum now. The nine coefficients
ride in the frame block, the prefiltered cubemap and the table are set 0 beside it, and
`MeshPass` resolves the irradiance from the environment GUID under the kind word `irradiance`.
The lookup table comes through the manifest by source path, so no GUID is written into C++. A
scene that names no environment gets the irradiance of the grey fallback, which is pi times
`render::kFallbackCubeTexel`, so its diffuse and its specular come from one number.

`sandbox/content/models/spheres/` is a row of seven metal spheres from roughness 0.05 to 0.95,
because every other model in the sandbox is one material at one roughness and nothing showed the
difference. Deleting the mip choice, the coefficients, or the table each moves the picture 60 to
800 times the run-to-run noise floor, and each moves the surface it should: the table and the
mip move the metal spheres, and the coefficients move the dielectric helmet and barely touch the
metal.

M5.3a is the first half of the render graph. `src/render/render_graph.h` turns a list of pass
declarations into the barriers a frame needs, and it opens no device and names no Vulkan type.
`gfx::ResourceState` is the whole vocabulary, and it carries no catch-all value on purpose,
because a state meaning "anything" is what `ALL_COMMANDS` already is.

A pass declares its reads and writes as data rather than by calling a builder, so the
derivation is a pure function that `tests/test_render_graph.cpp` drives with no GPU. The three
terms of the barrier condition were each checked by deleting them: dropping the state compare
was caught by nothing until a test for a read that follows a read in a different state was
added. Nothing runs through the graph yet, which is issue #121.

M5.3b puts the frame through it. `begin_frame` no longer transitions anything, so both images
start in `ResourceState::Undefined` and the runtime issues what `derive_barriers` worked out.
`gfx::cmd_frame_barrier` is the one entry point, and `ALL_COMMANDS` is gone from every barrier
the engine issues. Presentation stays in `end_frame`, because that wait belongs to the present
semaphore.

`--sync-validation` turns on the check that reads barriers rather than calls. It found two real
races on the first run, and both are older than the graph. A transition out of `Undefined` used
`TOP_OF_PIPE`. That ordered it against neither the swapchain acquire nor the frame before, which
still had the one shared depth image.

Such a transition now waits on the stage the new state uses. A 300-frame run reports nothing.
The screenshot path still does, and issue #124 holds it. `capture_frame` waits for the device,
and presentation is not device work.

**Dynamic state carries from one pass to the next, and `cull_back` used to be a lie.** Culling
is dynamic on every graphics pipeline, so what a pipeline was built with counted for nothing.
`cmd_bind_pipeline` applies it now, which closed #188. Before that, a scene of opaque geometry
alone rendered pure black: the mesh pass left back face culling on, and the tonemap pass draws
one full-screen triangle whose winding is whatever the index arithmetic gives, so the triangle
was culled and the frame kept its black clear.

Nothing reported it. The validation layer was happy, because every call was legal. The
sandbox hid it for months, because the glass panes are double sided and they draw last, so
they turned culling off before the tonemap ran. Only a scene with no blended geometry showed
it, and the large test scene of #130 was the first one pointed at the engine.

The picture is the only thing that can catch this class of bug, and CI has no GPU. See #190.

The sandbox is an interior now. `sandbox/content/models/room/` is five coloured walls, generated
the way the spheres are, and everything else stands inside it. Open space could not test a
shadow, because nothing occluded anything, and it could not justify several lights either.

Two real scenes were measured and rejected. Khronos Sponza is `LicenseRef-CRYENGINE-Agreement`
and not the CC-BY it is widely assumed to be. Intel Sponza is CC BY 4.0 and its geometry is a
133 MiB `.bin`, which GitHub refuses. Issue #130 holds the large scene, fetched from outside
git.

The room colours were the usual Cornell values scaled to about a third until M5.6b, and that
scale was measured rather than chosen. At full values 35 percent of the viewport clipped to
white with **every light switched off**, because the image based ambient alone is enough. An
interior is what made that impossible to miss.

M5.6 is complete and the scale is gone. M5.6a put the scene on a half float target and added a
full-screen pass that wrote it out, applying no curve, so the picture did not move. M5.6b put
the ACES fit from Stephen Hill in that pass and made exposure a reflected `ViewSettings` field
that `view.json` saves and `--exposure` overrides. At the full Cornell values with every light
off, clipping is now 0.0018 percent, which is 17 pixels of specular highlight on two metal
spheres. Exposure scales the scene before the curve, because scaling after a curve that is not
a straight line throws away the highlight roll off.

`gfx::GraphicsPipelineDesc` gained `push_constant_stages` for that. A push constant reached the
vertex stage alone, and the exposure is applied where the curve is.

The Smith remapping is `alpha / 2` for image based lighting, where alpha is roughness squared.
`mesh.frag` uses `(roughness + 1) squared / 8` for direct light. Squaring alpha twice here left
the table reaching eight at a grazing angle, and only the energy test caught it: the mirror test
passed throughout, because at no roughness there is nothing to shadow.

The cooker half of the permutation work landed. A `.meta` sidecar carries a `shader` block with
a list of variants, and each names its defines. The shader rule numbers its parts the way the
glTF rule does, so `mesh.frag` gives `mesh.frag.0.shader`. Part 0 is the base form and it keeps
the identity of the source. The rest derive one under the kind word `shader`. A cooked module
records what it was built with, so a consumer picks by declaration rather than by manifest
order.

M5.1 is complete. `mesh.frag` is `#ifdef` now, `mesh.frag.meta` lists four variants, and
`MeshPass` picks one for each submesh from the maps the material named. The two toggles are the
normal map and the occlusion map, because every other map reads a white texel when the material
named none and needs no branch.

A sampler stays declared in the form that never reads it. A declaration inside an `#ifdef`
would give each form its own set layout, and then one material set could not bind against
another form. `build_pipelines` compares every form against the base form and refuses the set
when they disagree, because Vulkan calls that undefined rather than an error. The pass holds
eight pipelines, four forms times opaque and blended, and a reload rebuilds all of them or
keeps all of the old ones.

The opaque draws sort by pipeline variant, which closed #105. `pipeline_switch_count()` is what
measures it.

**M5 is complete.** #88 closed it: Intel Sponza renders correctly, at 3.75M triangles over 115
meshes and 28 materials, lit by a sun with four cascades, 22 lamp point lights, and image based
lighting from the sky the model ships with. `scenes/sponza/` holds the scene and
`scripts/fetch-scene.py` brings the model down, because 133 MiB of geometry in one file is more
than GitHub accepts. That was #130.

The frame is **20.3 ms** at 1280x720 on the reference GPU, repeatable to 0.5 percent over three
runs of 600 frames. Synchronization validation reports nothing over 300 frames of it. Measure a
later change against that number and against the per-pass split: shadow 8.07 ms, mesh 7.30 ms,
cull 0.025 ms, tonemap 0.162 ms.

The shadow pass costs more than the mesh pass, which the sandbox could never have shown. It draws
the world four times at 2048 square, and the cascade cull of #180 leaves 311 draws against the
mesh pass's 292. Issue #194 holds it, with the numbers to beat.

Two pieces of the M5 definition did not land. #122 is the aliasing half of the render graph, and
its trigger condition is still not met: the shadow map, the scene color and the cluster grid all
overlap at the mesh pass, so no two have disjoint lifetimes and there is nothing to alias.
Nothing draws the environment either, so open sky reads as the clear color, which is #193. The
sandbox is a closed room and never showed it.

Sponza also carries its own lights, 24 of them under `KHR_lights_punctual`, and the cooker drops
every one. The lamp positions in `scenes/sponza/main.scene` were read out of the glTF by a script
and written in by hand. Issue #192 holds that, and the reason it is not trivial: every intensity
in that file is 0.0, and glTF measures a point light in candela while `scene::PointLight` carries
no unit at all.

The mesh cull left one of its own. #177 is the tighter test, per submesh rather than per entity. It
covers both passes, and it needs a scene that can measure the difference.

M5.7d culls the shadow pass too, which closed #180. `ShadowPass::draw` extracts the six planes of
each cascade and tests every entity against them. It is the cascade volume and never the camera
frustum, because a mesh behind the camera still casts into a cascade.

The test cannot change the map. Nothing enables depth clamp, so the rasterizer already clips what
falls outside those planes. The cull drops the draw for geometry the GPU was going to throw away.

It pays better than the camera cull, for two reasons. The pass runs four times over the same world.
And a cascade covers a slice rather than the whole view. So the opening camera goes from 0.452 ms to
0.391 ms, and a camera turned away goes from 0.363 ms to 0.249 ms.

A cascade volume is orthographic, which is where both depth planes are real. Every earlier frustum
test drove an infinite perspective, whose far plane is degenerate. So none of them could see a wrong
depth pair. `tests/test_frustum.cpp` drives an orthographic volume now.

M5.7c culls a mesh as well as a light. `src/math/bounds.h` turns the local bounds of a mesh and a
world matrix into a world-space sphere, and `MeshPass::draw` tests it against the planes `cull()`
already extracted. One extraction serves both, so they cannot disagree about the camera. The
radius is exact for the transformed box, because the cheap form underestimates when the matrix
columns lean the same way and an underestimate drops a mesh that is on screen.

The sandbox proves it rather than measuring it. Every entity is in view from the opening camera, so
nothing culls and the picture does not move. Turning the camera 45 degrees drops 5 of 27 entities
and the picture is still byte for byte identical. Turning it 180 drops 22.

The saving here is small. Against the same camera the mesh pass goes from 1.536 ms to 1.510 ms at
45 degrees, and from 0.094 ms to 0.002 ms at 180. A GPU rejects an off-screen triangle cheaply, so
the cull buys the draw call and the vertex work rather than fragment time, and the sandbox has too
little of either to show it. Comparing two different cameras is what overstates it: 1.46 ms looking
at the room against 0.002 ms looking away is mostly the camera, not the cull.

The test is per entity, and #177 holds the tighter per-submesh one.

**M7 is complete.** Box3D runs on the enkiTS pool, `RigidBody` and the colliders are reflected
components, and `physics::Simulation` turns them into bodies and moves the entities that follow.
`engine::FixedTimestep` in `src/core/timestep.h` steps the solver at a fixed rate whatever the
frame rate, and `Simulation::interpolate` blends the last two steps into the pose a frame draws.
`physics::World::debug_lines` reads the wireframe out of Box3D and `render::DebugLinePass` draws
it, behind `--physics-debug` and a reflected `ViewSettings` field.

The milestone test passes: a stack of three crates stands in the sandbox room and a crate thrown
from the camera knocks it over. `--throw-at-frame <n>` fires the same throw on a fixed frame, so
an offscreen capture of it is reproducible.

**The game logic runs on the fixed step**, which closed #245. `sandbox::update` takes simulated
seconds rather than the wall clock, so a run is reproducible.

`scene::StepMotion` is where the interpolation for a non-physics mover lives. It records the two
poses a frame blends, the same way `physics::Simulation` does. `begin_step` puts the
authoritative pose back before the game reads it. Without that, the motion of a frame between
two steps feeds back in and compounds. It also drops any entity the world no longer holds,
because a reload recycles entity numbers.

The same wall time is not the same number of steps. In float, 288 frames of 1/144 sum to just
under two seconds. That run takes 119 steps where a 30 Hz run takes 120. So a determinism test
holds the step count fixed, not the wall time.

**A collider is multiplied by the world scale of its entity**, which closed #237 after the
milestone. So one crate prefab makes a big crate and a small crate. A box takes a scale of three
different numbers exactly. A sphere holds one radius, so it takes the largest of the three and
warns, naming the entity. A scale changed after the body exists rebuilds the shape and keeps the
body, so a crate resized while it falls keeps its velocity. The compare carries a tolerance,
because a matrix decompose leaves rounding that moves with the rotation, and an exact compare
rebuilds a resting body every step and it never settles. `Simulation::shape_rebuild_count`
measures that. `sandbox/content/main.scene` stands a crate at 1.6 beside the stack, and
`--physics-debug` shows its wireframe covering the mesh it draws.

**The simulation is deterministic.** Three offscreen runs of the same command, with the solver on
eight workers and a crate thrown on a fixed frame, produce byte-identical images. Working the
step count out by dividing rather than subtracting cost a step of every second, because 1/60 is
not a number a float holds: one second divided by it gives 59.99998 and truncates to 59.

Box3D turned out to need nothing patched. What it did cost is four things that had to be read
out of its source: gravity is -10 rather than -9.8, a sleeping body ignores a velocity of zero
silently, the debug wireframe of a shape is cached by the application through a callback on the
world definition, and a hull stores half-edges so every edge is in the array twice. `DESIGN.md`
section 5 holds all four. M8.4 found three more, so that section now carries seven.

**M8 is complete, and the sandbox game is Lua.** M8.0 put input in `platform/`, because every
read sat inline in `apps/runtime/main.cpp` and a script API over that would have inherited an
SDL shape. M8.1 added the host and the cook rule, M8.2 gave a script any reflected component by
name, and M8.3 the curated surface: the world, prefabs, physics, input, math and logging, each
chosen one call at a time.

M8.4 built the two things physics owed the milestone. A trigger volume is a reflected collider
that reports an overlap and pushes nothing, and a contact reports what touched what. Both report
on the step where they happened, and one step reports all of them. **A trigger overlap goes to
the volume alone and a contact goes to both bodies**, because a trigger has a direction and a
contact has none that Box3D promises. `--physics-debug` shows a trigger in wheat, which needed
no engine code: Box3D gives a sensor a broadphase proxy like any other shape and colors it
differently, and neither half is documented.

M8.5 is hot reload. **A reload restarts the script and the script table goes with it.** Carrying
a table across two versions has no answer for a value whose shape changed, so the rule is that
the script table is scratch and a component is storage. A component survives a reload untouched,
and it is saved by the scene file and shown in the inspector besides. A text that will not
compile keeps the old one running and reports once.

M8.6 closed it. `sandbox::update` and `throw_crate` are both gone, and `tests/test_sandbox.cpp`
drives cooked `.lua` through the callbacks because there is no C++ entry point left to call.

**Authoring the game found four engine gaps that C++ had hidden.** A script could not reach the
camera. A prefab a script instanced got no body, because the simulation reads the world once at
build. A dynamic body ignores a transform written from a script, so `Simulation::teleport` is
the only way to move one. And a destroyed entity left a stale body that killed the next step on
an EnTT assertion.

**Input edges and the fixed step are on different clocks.** A press edge is the difference
between two `Input::update` calls, and a frame often runs no step at all. Offscreen it almost
never does, so an edge raised and cleared between two steps is one the game never sees. The
runtime folds every device frame into what the next step reads and keeps a second
`platform::Input` on the step clock.

**A script writing a Transform to a dynamic body freezes it, and everything else reads as
working.** The write registers the entity with `StepMotion`, whose `begin_step` then restores
that pose every step. The body keeps integrating, the velocity reads back correctly, and only
the position stands still. `teleport` is the right verb. Issue #284 holds the warning that does
not exist yet.

Verified on 2026-08-13 with Clang 19, CMake 3.28.3, and Conan 2.31.1, on an NVIDIA
GeForce MX250 with the Khronos validation layer active. A texture and a scene reloaded
together in a running program, with no validation message. Two blended panes drew over the
opaque scene with no validation message either. Synchronization validation reports nothing over
300 frames offscreen and 200 windowed, on the sandbox and on Intel Sponza, and nothing over 300
offscreen frames with the physics wireframe drawing.

**The simulation is still deterministic with the game in Lua.** Three offscreen runs of the same
command, with a crate thrown on a fixed frame and the solver on eight workers, produce
byte-identical images. `math.random` is seeded from a fixed value and the clock-seeded spelling
is taken away, so a script cannot undo that by accident. `pairs` is the one thing an author has
to know about: Lua 5.4 seeds its string hash from the clock, so a string-keyed walk is not
reproducible. Use `ipairs` and a list when the order decides anything.

**M9 is in progress. M9.1 gives `apps/editor/` a program that builds and runs.** It is the
shell and nothing more: a window, the ImGui overlay, a menu bar, and a dockspace over the
whole work area. `ENGINE_WITH_EDITOR` is defined for that target and for no other one.
`with_editor=True` used to fail the configure, because the directory was not there and no CI
job ever turned the option on. Two jobs turn it on now.

Docking and the saved layout are per-application settings, so `gfx::ImGuiDesc` carries them
and `gfx::` decides neither. The editor asks for both and the runtime overlay asks for
neither. `platform::preferences_directory` is where the editor's `imgui.ini` goes, which is
`~/.local/share/camina/editor/` on Linux. A layout survives a restart, and the runtime
overlay still writes no file at all.

**M9.2 moved the panels into `src/editor/`, and both applications draw them.** The view,
the hierarchy, the inspector, and the material block table are one copy in `engine_core`
now, not code inside the `main.cpp` of one application. They are not behind `WITH_EDITOR`,
because the inspector has been the runtime debug overlay since M2. `DESIGN.md` §6 holds the
reasoning.

A panel places itself nowhere. The runtime overlay calls `place_next_panel` for its fixed
layout, and the editor docks. A panel header names no ImGui type, so a program that never
opens a window still compiles.

The editor opens the cooked content, reads the scene, and shows 42 entities of the sandbox.
The World panel saves the source scene, and that round trip is clean: a save, a cook, and an
offscreen capture give back the same image byte for byte. The written file is much longer
than the hand-authored one, because a save writes the whole instance record rather than the
short form a person typed.

A first run with no `imgui.ini` builds a default layout through the ImGui dock builder.
Without it all three panels open at one spot and bury each other. **Read both outputs of
`DockBuilderSplitNode`.** A node that has been split is a parent, and docking a window into
a parent rather than into a leaf leaves the window floating where it started.

**M9.3 draws the scene into a Viewport panel.** M9.3a moved the shadow, cull, mesh, and
tonemap passes out of `apps/runtime/main.cpp` into `render::SceneRenderer`, so both
applications draw a scene through one copy of the pass order and the barriers. M9.3b gave
the editor its own target: `editor::Viewport` owns the image the scene is tonemapped into,
and the panel shows it at the size a person dragged the edges to.

**M9.4 plays the scene in the editor.** M9.4a lifted the fixed step into
`engine::play::Session` in `src/play/`, which owns the clock, the solver, the script host,
and the input on that clock. The runtime drives it and its picture did not move by one
byte. M9.4b added `editor::PlayMode`: play snapshots the world with `scene::save_scene`,
stop reads that document back, and pause holds the steps without dropping the session.
`tests/test_editor.cpp` drives the shipped sandbox scene through a session that throws a
crate, and the stopped world writes out the same document it started from.

**Two traps the restore has to respect, and both are answered by ordering.** Stop drops the
session before it reads the world back, because writing a transform onto a live dynamic
body does nothing (#284). And both ends of a session replace every entity, so the selection
is dropped at each one: EnTT hands the same numbers out again.

**M9.5a moved the camera into the scene, and M9.5b gave the editor one of its own.** The
runtime draws through `scene::primary_camera` and steers that entity with
`editor::FlyCamera`. The editor draws through its own `FlyCamera` always, a running session
included, and reads the scene camera for one thing only: the pose a script sees. So flying
in the editor cannot move a level's camera.

The editor's view is saved to `camera.json` beside `imgui.ini`, under
`platform::preferences_directory`. `Describe<FlyCamera>` lists the pose alone, because the
speed and the sensitivity are application preferences and live in `ViewSettings`.

**M9.5a in detail.** `scene::Camera` is a reflected component, the
pose is the entity's transform, and `scene::primary_camera` picks the one a game plays
through. `editor::ViewSettings` keeps only the fly speed and the simulation rate, and
`view.json` no longer carries a camera at all. The sandbox scene carries a camera entity,
and both applications draw through it.

**The camera choice is by entity, not by the order EnTT hands them over.** A view iterates
a pool, and that order is neither creation order nor stable across a component being added
or removed. A scene file builds entities in the order it lists them, so the smallest entity
is the first camera somebody wrote. A test with two primary cameras caught this.

**That change moved the picture by 133 pixels of 921,600, by at most 2 of 255.** The view
matrix is the inverse of the world matrix now rather than a `lookAt` built from a yaw and a
pitch, and those two round differently. Every differing pixel is on a geometry edge. A wrong
pose moves the whole frame, so this is the shape of a rounding change rather than a mistake.
Measure a later camera change against `--offscreen --frames 120` and expect byte equality
from here on.

**M9.5c draws the gizmo, and it needed its own projection.** ImGuizmo expects Y up in
normalized device coordinates and a finite far plane. The engine renders with an infinite
reverse-Z projection whose Y row is negated for Vulkan, so `apps::gizmo_projection` builds a
neutral one for the handles alone. Handles and picture agree because the X row is the same
either way and the two Y flips cancel.

`editor::place_entity` is the write path, and it is in `engine_core` rather than beside the
ImGuizmo calls so a test can drive it with no window. Two mutations prove that test: forget
to divide out the parent, and two checks fail; write the Transform component instead of
calling `World::set_local`, and three fail, because nothing recomposes the subtree.

**`editor --select <name>` and `--gizmo <move|turn|size>`** exist so a capture can show the
handles. A gizmo otherwise needs a hand on the mouse, and there is no way to inject one.

**The gizmo has no keyboard shortcuts**, because W, E and R fly the camera. Issue #325 holds
the decision that would free them.

**The game's key bindings are in `sandbox::bind_actions` now**, not in the runtime's
`main.cpp`. Two applications run this game, and two copies of the table would let one key do
two different things. The camera bindings stay with whichever application flies a camera.

The build produces no compiler warnings and no clang-tidy findings, over the whole tree with
the gate on. It carried sixteen warnings at M7.1, which was issue #179, and the lint gate was
reading almost nothing, which was issue #242. Both are closed.

The gate is live now, which #252 closed. `cmake/ClangTidy.cmake` asks for `clang-tidy-19` by
name and refuses to configure when that binary rejects either `.clang-tidy`. Turning it on
needed `cook_gltf` and `run_frames` split, because both were over the cognitive complexity
threshold. `add_primitive` in `tools/cooker/mesh.cpp` sits at exactly 25 against a threshold
of 25, so one more branch in it fails the build.

## Development flow

**Do all development work on a branch and merge it with a pull request.** Commit
straight to `main` only for a small documentation fix, a typo, a comment change, or a
one-line tweak. When you are not sure which one applies, use a branch.

`main` carries no branch protection rules, and the user does not want any. This flow is
a convention, not a lock. The user can set it aside at any time. Follow an explicit
instruction to work on `main`.

### The loop

**At the start of a milestone, before any code.** Read the `DESIGN.md` §10 definition, and
read the GitHub milestone description beside it. They are two depths of one decision and
they have to agree, so correct the milestone when it has drifted. See "Issue tracker" below.
Then create the tracker issues for the increments inside the milestone, and name them
`M<n>.<k> — ...`.

Do this first, so the work has a shape before the first branch exists. An increment that
turns out to be two increments gets split into two issues, not carried as one large branch.

**Then, for each issue, one at a time:**

1. Branch from **current** `main`, after `git pull`. Name the branch
   `<type>/<short-topic>`, for example `feat/vulkan-swapchain` or `fix/arena-alignment`.
2. Do the work. Commit on the branch in the conventional style, and commit as you go
   rather than at the end. A commit is cheap and losing an afternoon of edits is not.
3. Open a pull request with `gh pr create`. Write the title in the conventional style,
   because a squash merge uses it as the commit subject.
4. **Start a monitor.** See "Monitoring a pull request" below. It is a regular source of
   mistakes, so follow that section rather than writing a fresh watch loop.
5. Read the automated review when it lands. Fix what is real. Say so, with a reason, when
   a finding is wrong. Push once, then monitor again.
6. When the checks are green and the review is either answered or absent, **post a summary
   of the work and wait.** Do not merge.

**Ask the user before you merge. Never merge your own pull request.**

### One issue, one branch, one pull request

**Open the pull request at the seam, not at the end of the issue.** An issue whose work
splits cleanly in half is two pull requests. M5.1 was the cooker half in #103 and the
renderer half in #106, and each was reviewable on its own. Holding both on one branch
would have made one large review of work that was already finished.

The test for a good seam is whether the first half stands up alone. A cooker change that
alters no pixel and carries its own tests is a seam. Half a shader rewrite that leaves the
sandbox rendering wrongly is not, and that half has to stay with its other half.

Several commits on one branch are not the same thing as several pull requests. Commit
often. Submit at the seam.

### A finding during the work becomes an issue, not a detour

When you find a bug, a shortcut, dead code, or a question the current work does not answer,
**file an issue and carry on.** Do not fix it on the current branch.

The exception is narrow: fix it here when it blocks this branch, or when it is genuinely
part of the issue you are working on. A texture format that cannot express a cubemap blocks
an environment rule, so it belongs. An unrelated overflow two functions away does not.

The rules for such an issue are under "Issue tracker" below. A finding that lives only in a
chat reply or a pull-request comment is lost.

### Writing a pull request body

Write it for somebody who has only the repository. They did not see the conversation that
produced the work, and they never will.

Put it in this order:

1. **What the pull request does.** First, in a sentence or two.
2. **What it gives, or why it was needed.**
3. **How to verify it**, when a reviewer can check something themselves.
4. Any other context that matters.
5. Issue links last. Use `Closes #<n>` when it closes one.

Rules:

- **Do not list files or paths.** GitHub shows the diff. Name a file only to point at
  something specific inside it.
- Keep it short. Prefer a list to a paragraph, and a sentence to a list.
- Follow the writing style below. Relaxed STE applies here.

**When you push again to an open pull request, say only what changed and why.** Do not
restate the branch.

**Keep `gh` current.** An old `gh` asks GitHub for `projectCards`, which GitHub retired in
2024. `gh pr edit` and `gh issue view` then fail with a deprecation notice that reads like a
warning, and the edit does not happen. 2.45 fails this way and 2.97 does not. Ubuntu ships
2.45 and never moves it, so install `gh` from the GitHub apt repository rather than from the
distribution.

### Collect every change before you push again

Gather the CI failures, the review comments, and any work you still owe the branch. Fix
them together, and push once.

This matters because a push restarts every CI job and starts a new review. Two pushes five
minutes apart cost two full runs and two reviews, and the second review reads a branch the
first one already covered. Waiting costs nothing, because the review has to arrive before
the branch is ready either way.

Squash merge is the default here. `cliff.toml` skips merge commits and strips the
`(#12)` suffix that GitHub adds to a squashed subject, so one pull request becomes one
changelog entry. A merge commit works too, but then every work-in-progress commit on the
branch reaches the changelog.

**No co-author trailers.** Do not add a `Co-Authored-By:` line to any commit message.
This overrides the default Claude Code behavior. The author of a commit is the person who
owns the repository.

Change `version.txt` in its own small commit on `main`, or in a release pull request of
its own. A push to `main` that changes `version.txt` starts a release.

## Monitoring a pull request

Every mistake in this section has actually happened. Follow it exactly.

**Start a background monitor as soon as the pull request exists.** Watch for two separate
things: the checks finishing, and the automated review arriving. They are unrelated
signals, and the review usually lands after the checks.

### Watching the checks

`gh` here is 2.45 and **`gh pr checks` has no `--json` flag**. A watch loop built on it
writes its error to stderr, reads empty input, never becomes true, and spins silently until
timeout. Silence looks exactly like "CI is still running".

Use `gh pr view`, which does support `--json`, and gate on `status`:

```bash
gh pr view <N> --json statusCheckRollup \
  --jq '[.statusCheckRollup[] | select(.name != null) | {name, status, conclusion}]'
```

Two traps, both of which have produced a false green:

- **A running check reports `conclusion` as `""`, not `null`.** So `all(.conclusion != null)`
  is true while the builds are still going. **Gate on `status == "COMPLETED"`**, which is
  unambiguous, and read `conclusion` only after that.
- **A bare `length > 0` exits far too early.** The review bot registers before the workflow
  jobs do, so the loop sees one finished check and reports success before the build starts.
  Require the expected count, which is **seven**: `format`, `docs`, `containment`, and
  four `build` jobs. The build matrix is two platforms times both options off and both on,
  and the job names carry which, for example
  `build (linux-clang, ui=true, editor=true)`.

Make the loop print the per-check result it decided on, so a wrong exit is visible in the
event rather than hidden behind the word "success". After the monitor reports, **query the
state once by hand** and report from that, not from the monitor's summary.

### Reading the review

**The automated review is a nice-to-have. It is not a gate.** CodeRabbit is an extra pair
of eyes, and the user reviews every pull request either way. So a pull request without an
automated review is a normal pull request, not a blocked one.

Read it when it arrives, and answer it. Do not treat its absence as a problem, do not
apologize for it, and do not hold work back for it.

A review is spread over three endpoints. Checking one or two and finding nothing proves
nothing:

```bash
gh api repos/<owner>/<repo>/pulls/<N>/reviews    # the review and its summary
gh api repos/<owner>/<repo>/pulls/<N>/comments   # inline comments, anchored to a line
gh api repos/<owner>/<repo>/issues/<N>/comments  # top-level, where the walkthrough goes
```

`gh pr view <N> --json reviews` covers only the first. The walkthrough lands on the
**issues** endpoint, which is easy to miss because the subject is a pull request.

**An empty result means "not yet", never "clear".** Say that the review has not arrived.
Never treat it as a pass.

**CodeRabbit does not review this repository on its own.** It posts a top-level comment
saying the review is available on request, because it asks for a manual trigger on a
repository with fewer than 10 stars. So a pull request here normally gets no review at
all, and that is the expected state rather than a fault.

Read that comment for what it is and carry on. **Do not comment `@coderabbitai review`
without being asked.** Triggering it writes a public comment on the user's repository,
and whether an automated review is worth having on a given pull request is their call.
Say in one line that no review will run unless they ask for one.

**A rate limit ends the wait.** CodeRabbit posts its own limit as a top-level comment, with
a reset time. When it does, stop the monitor, say that the automated review will not run
for this pull request, and carry on. Do not wait for the reset, and do not poll for it.

A rate limit says nothing about the pull request. It means one automated tool was busy,
and that tool is a nicety. Report it in one line, in the same way you report that a run
used no cache. Whether to ask for the review later is the user's call, not yours.

### Check the review surfaces before every push to an open pull request

**A review can arrive at any time, so read all three endpoints immediately before
you push again.** This is not the same as reading the review once when it lands. A
push that ignores waiting comments looks like an answer to them, and the next
reviewer cannot tell which findings were considered and which were missed.

This matters more now that the review is triggered by hand. The review does not
arrive on a schedule you can predict from the push, and it may land hours later,
against a commit that is no longer the head of the branch.

So the order is: gather the CI failures, read the three endpoints, decide on every
finding, then push once.

### Answering the review

Verify each finding against the code before acting on it. A finding can be wrong, and
several have been. When one is wrong, say so with evidence rather than with an argument:
measure the file, run the check, quote the line. When one is right, fix it and say what the
failure would have been.

Then push once and start the monitor again.

**Stop the old monitor before you start the new one.** A push begins a fresh run, and a
monitor still watching the previous one keeps reporting against a run that no longer
matters. Two monitors on one pull request send overlapping events for different runs, and
the older one can announce a result for work you have already replaced. Use `TaskStop` with
the task id the monitor reported when it started.

## Issue tracker

`DESIGN.md` §10 defines the milestones. The GitHub tracker holds the state.

### `DESIGN.md` and the GitHub milestones are two halves of one thing

**`DESIGN.md` is the source of truth, and the GitHub milestones are not a copy of it.** They
carry different depths of the same decision, and they have to agree.

- **A GitHub milestone carries enough to understand the work without opening anything else.**
  What it builds, roughly how, and its done-when test. A person reading the tracker should
  know what the milestone is for and when it is finished. Keep it to a few sentences.
- **`DESIGN.md` §10 carries the detail.** The full definition, the reasoning, the named
  dependencies, and anything that had to be settled. When the two disagree, `DESIGN.md`
  wins and the milestone gets corrected.
- **Update both together.** A decision that changes a milestone changes the `DESIGN.md`
  section and the milestone description in the same pass. Half an update is drift, and drift
  here is quiet: a milestone description is read far more often than it is checked.

Drift is real and not hypothetical. The M10 description said its foundation was M5.5, which
is cascaded shadow maps, while `DESIGN.md` said M6, the moth_ui spike. Nothing failed. It
just told the wrong story to anybody reading the tracker.

Check the two against each other when you start a milestone, which is when the cost of a
wrong description is highest:

```bash
gh api repos/instinkt900/camina/milestones --jq '.[]|"\(.title)\n  \(.description)\n"'
```

### Issues

- **One GitHub Milestone for each `DESIGN.md` milestone**, M0 through M11.
- **Issues are work increments, not milestones.** M1 was one line in `DESIGN.md` and became
  three pull requests. Split a milestone the same way, and name the issues `M<n>.<k> — ...`.
- **An issue links to its `DESIGN.md` section. It never copies the definition.** Two copies
  drift. The issue body holds the task list and the state. This is the one place a copy is
  wrong: a milestone summarizes, and an issue points.
- **Create issues for the milestone in progress and the next one.** A detailed ticket for
  M9 written today will be wrong by the time it starts.
- Labels are `area: build`, `area: gfx`, `area: render`, `area: core`, `area: assets`, and
  `area: editor`. Put `milestone-goal` on the issue that carries the milestone's own
  done-when test. Put `tech debt` on a known shortcut to pay back later.
- Reference the issue from the pull request, so GitHub links them.

**File an issue for every problem you find.** When you find a bug, a shortcut, dead code, or
a question that the current work does not answer, open an issue for it. Do this even when
you cannot fix it now, and even when the user has not answered yet. A finding that lives
only in a chat reply or in a pull-request comment gets lost.

Rules for such an issue:

- Say where you found it, with a `file:line` reference.
- Say why it matters. A finding with no cost attached is not worth tracking.
- Give a `Done when` checklist, so a later session can tell when it is closed.
- Add a milestone only when the work clearly belongs to one. Leave it empty otherwise.
- Do not fix it in the current branch unless it blocks that branch. File it and move on.

## Writing style

Write all prose in relaxed "STE-flavored" ASD-STE100 Simplified Technical English. Use the
`ste-writing` skill. This covers documents, READMEs, code comments, commit messages, and
pull-request text. Do not wait for a request.

The rules that matter most:

- Active voice. Name the actor.
- No semicolons and no contractions.
- Sentences under 25 words. One topic per paragraph.
- Plain verbs. Do not write "perform an analysis" or "spin up".
- One name for one thing.

Use strict mode only for procedures, runbooks, and error messages. The rules do not apply
to code, identifiers, or command syntax.

## Tooling

`.clang-format`, `.clang-tidy`, `.clangd`, and `cliff.toml` came from the moth_ui
project so that the two repositories agree. Keep them in step. When you change one
here, consider whether moth_ui wants the same change.

- **Format.** `.clang-format` is copied from moth_ui without edits. Namespace bodies
  indent, braced lists carry inner spaces, and there is no column limit. Run
  clang-format before you commit. CI fails on any diff.
- **Lint.** `.clang-tidy` starts from moth_ui and adds engine-specific entries at the
  end of the list. `tests/.clang-tidy` relaxes magic numbers for test code.
  `cmake/ClangTidy.cmake` runs clang-tidy in the compile step, but only in a Debug
  build or when `CI` is set. A missing clang-tidy fails the build in CI.
- **clang-tidy must be 19, and a wrong one fails open rather than loudly.**
  `ExcludeHeaderFilterRegex` needs clang-tidy 19. Version 18 rejects that key,
  and rejecting one key throws the whole file away: it prints
  `Error parsing .clang-tidy: Invalid argument` and carries on with its own
  defaults, which are `clang-analyzer-*` and the compiler diagnostics. No
  `Checks` list, and no `WarningsAsErrors`. So the build passes and the lint
  gate checked almost nothing.

  `cmake/ClangTidy.cmake` asks `find_program` for `clang-tidy-19` first, and it
  verifies both `.clang-tidy` files against the binary it picked. A rejected
  config stops the configure with a message. So this cannot come back silently,
  and it is checked in a local Debug build as well as in CI.

  **`--verify-config` needs `--config-file` or it proves nothing.** The bare
  form finds no `.clang-tidy` at all. It prints every parse error it hits and
  then reports `No config errors detected` and exits 0 anyway, from any
  directory. With `--config-file=<path>` the exit code is honest: 1 on a
  rejected file and 0 on an accepted one.

  Ubuntu noble ships 19.1.1 as `clang-tidy-19`, and the plain `clang-tidy`
  package points at 18. The GitHub runner is the same: it carries a
  `clang-tidy` alternative that `update-alternatives --install ... 100` does
  not outrank, so CI ran 18.1.3 while it installed 19.
- **An alias is a separate check, so turning one off leaves the other on.**
  `cppcoreguidelines-avoid-magic-numbers` is an alias of
  `readability-magic-numbers`. Disabling only the second left the first running
  under `cppcoreguidelines-*`, and a local build reported about thirty findings
  that CI never did. Both names are in the list now. Issue #242.
- **`.clang-tidy` trap.** Never put a comment inside the `Checks:` block. It is a YAML
  folded scalar, so a comment line without a trailing comma merges with the entry
  after it, and that entry stops working with no error. Put rationale above the key.
- **Third-party headers and clang-tidy.** `HeaderFilterRegex` is `.*`, so clang-tidy
  reads every header our code includes, including the ones in the Conan cache and the
  ones in `third_party/`. `ExcludeHeaderFilterRegex` drops both. `SKIP_LINTING` on a
  source file does not help here, because a third-party header arrives through our own
  translation unit. `WarningsAsErrors` is `*`, so this fails the build rather than
  printing. The ImGui Vulkan backend header is the case that needed the Conan half.
  Box3D needed the other one. `box3d/collision.h` alone reports 3305 findings, and most
  of them are C-style casts in inline accessors.
- **clangd.** `.clangd` points at `build/RelWithDebInfo`. There is no
  `compile_commands.json` symlink and none is needed.
- **clang-format** is not in apt on this machine. It lives in the Conan virtual
  environment: `~/.venv/conan/bin/clang-format`. It is version 19, and CI pins
  clang-format 19 as well. Do not use the apt package, which is 18. The two
  disagree on pointer-to-member spacing, so version 18 fails CI on code that
  version 19 calls clean.
- **MSVC and `__VA_OPT__`.** The old MSVC preprocessor has no `__VA_OPT__`, and it is
  still the default. `src/CMakeLists.txt` sets `/Zc:preprocessor` as a PUBLIC option
  for that reason. A C++20 macro that works with Clang can still fail on Windows
  without it.
- **ImGui backends.** ImGui ships its platform and renderer backends as loose source
  files, and the Conan package copies them to `res/bindings`. The recipe does not
  declare a resource directory, so `imgui_RES_DIRS` is empty and
  `cmake/ImGuiBackends.cmake` works from `imgui_PACKAGE_FOLDER_<CONFIG>` instead. The
  backend must see `IMGUI_IMPL_VULKAN_USE_VOLK`, because the engine never links the
  Vulkan loader library. Rule 4.4 keeps these files in the Conan cache, not in
  `third_party/`, since we do not patch them.
- **Content trees.** There are two, and the cooker builds both into
  `<executable dir>/content/`. `src/render/content/` holds what `render/` reads, which is
  the two shaders and the cube texture. `sandbox/content/` holds the game scene and its
  prefabs. `runtime --content <dir>` overrides the game one.
- **Shaders.** `tools/cooker/` compiles GLSL to SPIR-V by running glslc, and writes the
  module as a file next to the executable. glslc arrives through the `shaderc` tool
  requirement in `conanfile.py`. Conan Center has no binary for our profile, so the first
  install builds shaderc, glslang, and SPIRV-Tools from source. That takes several minutes
  once, then the cache serves it. Issue #43 holds the reasons to link `libshaderc` instead.
- **Submodules.** There are three: `third_party/bc7enc_rdo`, `third_party/box3d`, and
  `third_party/imguizmo`. A fresh clone needs `git submodule update --init --recursive`.
  Both `third_party/bc7enc/CMakeLists.txt` and `third_party/CMakeLists.txt` fail the build
  with that command in the message when the directory is empty. Only `bc7enc.cpp` is
  compiled out of bc7enc_rdo. The rest of that repository holds an ISPC kernel, a PNG
  reader, and a DDS writer that we do not use.
- **ImGuizmo is vendored because no Conan package can be used**, which closed #308.
  `imguizmo/1.83` and `imguizmo/cci.20231114` both require `imgui/1.90.5`, and this engine
  is on `1.92.8-docking`, so Conan refused the graph and `with_editor=True` would not
  configure. The submodule compiles against 1.92 with no patch. Only `src/ImGuizmo.cpp` is
  built, behind `ENGINE_WITH_EDITOR`, and the editor is the only target that links it. Rule
  4.4 was widened to say this: vendor what Conan cannot give you, not only what you patch.
- **Box3D builds from `third_party/box3d/src`, not from the Box3D root.** The root
  CMakeLists is written for a top-level build. It sets the static MSVC runtime, and
  `profiles/windows-msvc` asks Conan for the dynamic one. MSVC does not link objects that
  disagree about the runtime. The root also turns on verbose makefiles and pulls in
  FetchContent. `third_party/CMakeLists.txt` carries the one flag the root sets that the
  library needs. See `DESIGN.md` §5.
- **Third-party sources we compile.** `SKIP_LINTING` is a source file property, not a
  target property. Setting it on a target does nothing and reports nothing.
  `gfx/vulkan/vk_vma.cpp`, `tools/cooker/stb_image_impl.cpp`, `bc7enc.cpp`, and every
  Box3D source all carry it on the file, with `-w` alongside. A source file property
  belongs to the directory that declared the file, so the Box3D sources need the
  `DIRECTORY` form of `set_source_files_properties`.
- **EnTT assertions.** `src/core/entt.h` points `ENTT_ASSERT` at `ENGINE_ASSERT`.
  Include it before any EnTT header. Every engine header that includes one does that
  already, and the file fails the build with a message when the order is wrong.
  Without it EnTT falls back to `assert()`, which `NDEBUG` removes from a
  RelWithDebInfo build, so a `get<T>()` for a component that is not there kills the
  process with no message.
- **CI builds RelWithDebInfo only.** The matrix is Linux with Clang and Windows with
  MSVC, both RelWithDebInfo. Debug does not join it, because it costs more CI time
  than it returns. So Debug is a configuration a person builds when they need it, and
  it can break without anybody hearing. Build it before you trust it.
- **A Debug build that fails in `pulseaudio` is a poisoned Conan cache, not this
  repository.** Debug packages built elsewhere on the machine with a sanitizer record
  only `build_type=Debug` in the package ID, so Conan reuses them. The downstream package
  then links without the sanitizer flags and the symbols are missing.

  The failure walks: `libmp3lame` → `libsndfile` → `wayland-scanner` → `wayland`.
  Removing one package with `conan remove "<pkg>/*" -c` moves the failure to the next,
  because several were poisoned together.

  **The fix is to remove every affected package at once**, and then install again:

  ```
  conan remove "libmp3lame/*" -c
  conan remove "libsndfile/*" -c
  conan remove "wayland/*" -c
  conan remove "wayland-protocols/*" -c
  conan remove "sdl/*" -c
  conan install . -pr:h profiles/linux-debug -pr:b profiles/linux-debug \
    -s 'sdl/*:wayland=False' -b missing
  ```

  **`conan remove "<pkg>/*" -c` clears every configuration of that package, including
  RelWithDebInfo.** After the cleanup, the RelWithDebInfo build also needs a reinstall
  from source: `conan install . -pr:h profiles/linux-clang -pr:b profiles/linux-clang
  -b missing`. Nothing warns before this happens, and the next build fails on missing
  libraries. Nothing in `profiles/`, `cmake/`, or `CMakeLists.txt` sets a sanitizer.
- **A test that must die.** ctest cannot express "this program must abort, and it must
  say why". `PASS_REGULAR_EXPRESSION` does not override a process that a signal
  stopped, and `WILL_FAIL` passes whether the message appeared or not.
  `tests/expect_assert.cmake` runs the program and checks both. It runs through
  `cmake -P`, so it needs no shell and works on both platforms.

## Documentation comments

Every public entity in a header carries a Doxygen comment, in the moth_ui style.
This is enforced: the docs build runs with `EXTRACT_ALL` off, `WARN_IF_UNDOCUMENTED`
on, `WARN_NO_PARAMDOC` on, and `WARN_AS_ERROR FAIL_ON_WARNINGS`. A new public
function with no comment, or a `@param` whose name does not match the signature,
fails CI.

Style:

- `/** @brief ... */` block for anything that needs more than one line. Put the
  brief first, then a blank comment line, then the detail.
- `/// @brief ...` for a one-line description.
- `///< ...` after a member for a trailing description.
- `@param` for every parameter, `@return` for every non-void return, `@tparam` for
  every template parameter.
- `@warning` for a trap the caller must know about, `@code` for a usage example.
- Each header opens with a `@file` block. Macros need it, because Doxygen will not
  document a file-scope macro otherwise.
- Document each namespace once, in the header where it first appears. Documenting
  it twice is a warning.
- `.cpp` files are excluded from the docs. They hold implementation comments, not
  API documentation.
- The docs read `src/` only. `sandbox/` is a game that consumes the engine, not part
  of the engine interface, so its headers are not checked. They carry the comments
  anyway, because the game is also the worked example.
- Wrap a construct Doxygen cannot parse in `/// @cond` and `/// @endcond`, with a
  comment saying why. `std::hash<Handle<Tag>>` in `src/core/handle.h` is the one
  current case.

Check the docs without a Conan install:

```bash
cmake -S . -B build-docs -DCAMINA_DOCS_ONLY=ON
cmake --build build-docs --target docs
```

## Versioning and releases

`version.txt` at the repository root is the single source of the version. Both
`CMakeLists.txt` and `conanfile.py` read it. CMake generates `src/core/version.h`
from `src/core/version.h.in`, which is generated and git-ignored.

To cut a release, change `version.txt` and push to `main`. `.github/workflows/release.yml`
then tags the commit, generates `CHANGELOG.md` with git-cliff, and publishes a GitHub
release. Write commits in the conventional style (`feat:`, `fix:`, `refactor:`) so
that `cliff.toml` groups them. Plain commits still appear, under "Changes".

## Hard rules

`DESIGN.md` §4 holds the full text and the reason for each rule. Do not break these.

1. **Contain Vulkan.** Only files under `src/gfx/vulkan/` can include `vulkan.h`, volk,
   or VMA. Every layer above uses `gfx::` types. CI enforces this with a grep.
   `src/render/` is the layer above and holds the render graph and the passes.
2. **Keep `gfx::` C-compatible.** No `std::string`, no `std::vector`, no virtuals, and no
   exceptions in the public `gfx::` interface. Use opaque generational `uint64_t` handles
   and POD structs.
3. **`WITH_EDITOR` removes code. It never changes code.** `editor` and `runtime` are two
   executables over `engine_core`, and the game module links into both. The macro may
   remove an editor-only method, member, subsystem, or attribute. It must never change
   what the remaining code does, because then a shipping build behaves unlike the editor
   build.
4. **Vendor only what you patch.** Everything else comes from Conan.
5. **Reflect once, consume many times.** Every field enumeration goes through `reflect/`.
   Do not add a second descriptor system.
6. **The sandbox game sets the scope.** Build a system when `sandbox/` needs it.

## Conventions

Wrong conventions produce mirrored or inverted geometry, and the cause is hard to find.
`src/math/conventions.h` holds these once the code exists. `DESIGN.md` §3 holds the full
table.

- Right-handed, +Y up, −Z forward. This matches glTF.
- Meters, kilograms, seconds.
- Column-major matrices. `GLM_FORCE_DEPTH_ZERO_TO_ONE` and `GLM_FORCE_RADIANS`.
- Reverse-Z depth. Near is 1 and far is 0.
- Quaternions in `wxyz` order.
- Linear color working space. Convert sRGB at texture read and at final write only.

## Checking the picture

**Always capture with `--offscreen`. Never open a real window to take a screenshot.**

```bash
./build/RelWithDebInfo/bin/runtime --offscreen --resolution 1280x720 --frames 120 \
    --no-watch --screenshot out.png
```

This is not a preference. A windowed capture is whatever size the window manager decided,
and that size changes when somebody moves a window or changes a layout while the run is
going. A capture taken that way once read as a rendering regression when the only thing
that had changed was the desktop. A windowed run also steals focus and disturbs whoever is
using the machine.

Two offscreen runs of the same command produce the same image, texel for texel. So a
comparison between them has **no noise floor to subtract**, and a difference of one pixel
is a real difference. That is what makes a mutation test worth running:

1. Capture the scene as it is.
2. Break the one thing that is supposed to matter.
3. Capture again and compare. Any difference at all is the effect of that thing.

Two rules that follow from how it works:

- **`--resolution` is exact only offscreen.** Windowed it is a request, and a tiling window
  manager ignores it. A run that has to be reproducible needs `--offscreen` as well.
- **An offscreen capture has no ImGui overlay**, because the backend needs the window. That
  makes it the more sensitive image, and it means an offscreen capture and a windowed one
  cannot be compared against each other. Pick one and stay on it.

Run the windowed path as well before you trust a rendering change, because that is what
ships. `--sync-validation` belongs on both.

## Build

Three configurations exist: RelWithDebInfo (development), Debug (crashes), and
Release (shipping). The profile sets the build type for the first two. Release
overrides it from the command line.

```bash
# RelWithDebInfo: optimized with symbols and assertions.
conan install . -pr:h profiles/linux-clang -pr:b profiles/linux-clang -b missing
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
ctest --preset conan-relwithdebinfo --output-on-failure
./build/RelWithDebInfo/bin/runtime --frames 300
```

```bash
# Debug: no optimization, assertions on, clang-tidy runs locally.
conan install . -pr:h profiles/linux-debug -pr:b profiles/linux-debug -b missing
cmake --preset conan-debug
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure
```

```bash
# Release: optimized with assertions off. The profile is still linux-clang, and
# the build type is overridden here.
conan install . -pr:h profiles/linux-clang -pr:b profiles/linux-clang \
  -s build_type=Release -b missing
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
```

Windows uses `profiles/windows-msvc` and `profiles/windows-debug` from a
"x64 Native Tools Command Prompt for VS 2022". All profiles ask for the Ninja
generator, so the preset names match on each platform. Two things differ on
Windows. An MSVC build skips clang-tidy, because clang-tidy reads a clang
command line. The rule 4.1 test needs bash, so it does not run without Git Bash.
The Linux CI jobs cover both.

Conan lives in a virtual environment at `~/.venv/conan`. Add `~/.venv/conan/bin` to
PATH, or call the binary by its full path.

Conan options: `with_editor`, `with_ui`, `with_lua`, `with_audio`. Every one defaults
to False and turns on at its milestone. `with_ui` needs moth_ui in the local Conan
cache, because it is not on Conan Center.

**Turning an option on does not reach a build directory that already exists.** Conan
writes each option into the toolchain as a cache variable, and CMake keeps the value
the cache already holds. So `conan install -o "&:with_editor=True"` followed by
`cmake --preset` reports success and builds nothing new. Pass the variable on the
configure line as well, or delete the build directory:

```bash
cmake --preset conan-relwithdebinfo -DENGINE_WITH_EDITOR=ON
```

CI never sees this, because a runner starts from an empty checkout.

ImGui itself is not behind `with_editor`. Hard rule 3 says the editor is an application
and not a build mode, and the M2 inspector runs as a debug overlay in the runtime. The
option gates the editor application, and it gated ImGuizmo until M9.1 took that
requirement out. Every ImGuizmo recipe on Conan Center pins `imgui/1.90.5`, which
conflicts with the docking branch, so asking for it stopped the option resolving at
all. Issue #308 holds it, and M9.5 needs the answer.

Note: the enkiTS package installs its headers under a subdirectory. Include
`<enkiTS/TaskScheduler.h>`, not `<TaskScheduler.h>`.

## Dependencies

Take dependencies from Conan 2 by default. Vendor a dependency in `third_party/` only when
you may need to patch it. Today that means `box3d` and `bc7enc_rdo`.

`DESIGN.md` §5 lists every package and every rejected option.

## Settled questions

These decisions are made. Do not raise them again unless the user asks.

| Topic | Decision |
|---|---|
| Platforms and compilers | Linux with Clang, Windows with MSVC. GCC was dropped and is not a target |
| Render backend abstraction | Vulkan direct now. The plugin ABI comes later. Rules 1 and 2 keep it cheap |
| 2D support | Out of scope. Do not suggest Box2D |
| Package manager | Conan 2. Not CPM, and not vcpkg |
| Job system | enkiTS. Asio was considered and rejected |
| Reflection | Hand-written descriptors. Boost.Hana was considered and rejected |
| Shader compiler | shaderc with GLSL. No DXC |
| Mesh import | cgltf and glTF only. No assimp |
| Material authoring | Code-authored GLSL plus a reflected parameter block. A graph layer can sit on top later |
| Game UI | moth_ui. ImGui is for the editor and debug overlays only |
| Lighting architecture | Clustered forward. Deferred was considered and rejected. See `DESIGN.md` §9 |
| Debug in CI | No. RelWithDebInfo and the two platforms only. Debug costs more CI time than it returns |
| Networking | Not being built. Keep the three enabling decisions in `DESIGN.md` §9 |

## Sequencing

- Build M4, the asset pipeline, before M5, the renderer. A renderer with no asset pipeline
  draws only programmer cubes.
- Start `sandbox/` at M3 and keep it working.
- Keep the M6 moth_ui spike timeboxed. Its value is interface feedback, not pixels.
