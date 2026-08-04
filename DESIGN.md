# Camina Engine — Design & Roadmap

Status: M4 complete, M5 next
Last updated: 2026-08-04

---

## 1. Goals

This is a small 3D game engine. It lets you build games without writing the base systems
again for each project. The shape is familiar: entity/component, an editor, and scripting.
The engine is not complete, and that is on purpose.

**Goals**

- 3D rendering with a physically based pipeline.
- An editor application and a runtime application over one shared core.
- Game logic in Lua, with hot reload.
- One reflection system that serves editor UI, serialization, and script binding.
- A size that one person can understand in full.

**Non-goals.** Revisit each one only when a real game needs it.

- 2D as a first-class target.
- Networking.
- Console platforms.
- The same feature set as a commercial engine.

---

## 2. Fixed decisions

| Area | Decision |
|---|---|
| Language | C++20 |
| Platforms | Linux with Clang 19+, Windows with MSVC (VS 2022). One compiler for each. No GCC |
| Build | CMake 3.28+, Conan 2, Ninja |
| Graphics | Vulkan 1.3 direct. Dynamic rendering, synchronization2, no render pass objects |
| Backend abstraction | Deferred. The `gfx::` interface exists from day one. The plugin ABI comes later |
| Dimensionality | 3D only |
| Physics | Box3D (alpha), pinned to a commit |
| Jobs | enkiTS owns the worker pool. Box3D task callbacks use it |
| Windowing and input | SDL3 |
| ECS | EnTT |
| Scripting | Lua 5.4 with sol2 |
| Game UI | moth_ui (own library), optional dependency |
| Editor UI | Dear ImGui (docking) with ImGuizmo |
| Audio | miniaudio behind a thin interface, compiled in |
| Shaders | GLSL to SPIR-V with shaderc. No DXC |

### 2.1 The name

The project is **Camina Engine**.

**The C++ namespace stays `engine`.** This is deliberate, not an oversight.

Inside engine code, `engine::jobs::parallel_for` states what the thing is. A
`camina::` prefix would state only which product it belongs to, which the reader
already knows from the repository they are in. The namespace earns its place when
game code and engine code sit side by side, and there `engine::` still reads
better. A rename would also churn every file for no gain in clarity.

So the name appears in these places and nowhere else:

| Where | Value |
|---|---|
| CMake project | `camina` |
| Conan package | `camina` |
| Release artifacts and tags | `camina-<version>` |
| Window title, log banner | "Camina Engine" |
| Documentation title | "Camina Engine" |
| C++ namespace | `engine` (unchanged) |
| Target names | `engine_core`, `engine::core` (unchanged) |
| Macro prefix | `ENGINE_` (unchanged) |

The repository directory is still `~/Development/engine`. Renaming it is safe but
optional, and nothing in the build depends on it.

### Why the graphics abstraction waits

A backend abstraction with one implementation becomes Vulkan with different names. So the
abstraction waits until a second backend forces the interface to exist. The rules in §4
keep that later change mechanical instead of a rewrite.

### Why Box3D, and what it costs

Erin Catto released [Box3D](https://github.com/erincatto/box3d) on 2026-06-30. It is
portable C17 with no dependencies. It has a C API, SIMD, continuous collision detection,
multithreading hooks, triangle mesh and heightfield collision, and large-world support.
Catto describes it as an alpha that needs more tests and better documentation.

This has four results:

1. Pin a commit. Do not track `main`.
2. Vendor it as a submodule instead of the Conan package. You will read, patch, and update
   this dependency more than any other.
3. Link it direct. The C API removes all C++ ABI concerns.
4. Connect its `enqueueTask` and `finishTask` callbacks to the engine job system.

---

## 3. Conventions

**Decide these before you write any code.** Wrong conventions cause many bugs. The symptoms
are mirrored, inverted, or inside-out geometry, and they are hard to trace.

| | |
|---|---|
| Handedness | Right-handed |
| Up axis | +Y |
| Forward | −Z. This matches glTF, so import needs no conversion |
| Units | Meters, kilograms, seconds |
| Matrices | Column-major. `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_FORCE_RADIANS` |
| Depth | Reverse-Z (near = 1, far = 0), float depth buffer |
| Quaternions | `wxyz` storage |
| Texture origin | Top-left, the Vulkan convention |
| Color | Linear working space. Convert sRGB at texture read and at final write only |

These live in `src/math/conventions.h`. Record the reason for each one in a comment.

**Open item.** Check the default gravity vector and heightfield orientation in the Box3D
samples before you commit to +Y up. It almost certainly agrees. Confirm it anyway.

---

## 4. Rules

These rules hold for the life of the project. Each one keeps a later change cheap.

**4.1 — Contain Vulkan.**
Only files under `src/gfx/vulkan/` can include `vulkan.h`, volk, or VMA. Every layer
above uses `gfx::` types. CI runs a grep for violations and fails the build.

The backend sits under the interface it implements. `src/render/` is the layer above, and
it holds the render graph and the passes. `src/render/` never sees a Vulkan type.

**4.2 — Keep `gfx::` types C-compatible.**
The public `gfx::` interface uses no `std::string`, no `std::vector`, no virtuals, and no
exceptions. Use `const char*` with a length, a pointer with a count, opaque generational
`uint64_t` handles, and POD descriptor structs. This costs nothing now. It turns the later
plugin ABI into a header rename.

**4.3 — `WITH_EDITOR` removes code. It never changes code.**
`engine_core` is a library. `editor` and `runtime` are two executables that link it, and
the game module links into both. This is the Unreal and Hazel shape: the editor belongs to
the project and holds the project types, and it is not a generic tool that opens project
files.

`WITH_EDITOR` may remove an editor-only method, member, subsystem, or reflection
attribute. It must never change what the code that remains does. An `#ifdef` around a
branch inside a function that both builds run is a violation, because it lets a shipping
build behave unlike the editor build. That class of bug is miserable to trace, and this
rule is the only thing that prevents it.

The macro name matches the Conan option `with_editor` and matches Unreal, so one name
means one thing across the build, the package, and the code.

**4.4 — Vendor only what you patch.**
A dependency goes in `third_party/` only if you may need to patch it. Everything else comes
from Conan. Today this rule covers two entries.

**4.5 — Reflect once, consume many times.**
Every system that enumerates the fields of a type goes through `reflect/`. Do not write a
second descriptor system for serialization, editor UI, or script binding.

**4.6 — The sandbox game sets the scope.**
Build a system when `sandbox/` needs it, not before. This is the only reliable test that
separates what the engine needs from what was interesting to design.

---

## 5. Dependencies

### From Conan 2

| Package | Version | Use |
|---|---|---|
| `sdl` | 3.4.x | Window, input, gamepad, filesystem |
| `volk` | latest | Vulkan loader |
| `vulkan-memory-allocator` | latest | GPU allocation |
| `glm` | latest | Math |
| `entt` | latest | ECS |
| `enkits` | latest | Job system |
| `tracy` | latest | Profiler |
| `spdlog` | latest | Logging |
| `cgltf` | latest | glTF import, cooker only |
| `stb` | latest | Image read and write, cooker only |
| `meshoptimizer` | latest | Mesh optimization, cooker only |
| `shaderc` | 2025.3 | GLSL to SPIR-V |
| `spirv-reflect` | latest | Descriptor layout reflection |
| `imgui` | 1.92.x-docking | Editor UI |
| `imguizmo` | latest | Editor manipulators |
| `sol2` | latest | Lua binding |
| `lua` | 5.4.x | Scripting |
| `nlohmann_json` | latest | Serialization |
| `miniaudio` | latest | Audio, M10 |
| `ozz-animation` | 0.14.x | Skeletal animation, deferred |
| `moth_ui` | — | Game UI, optional, `with_ui` |

### Vendored in `third_party/` as git submodules

| Project | Reason |
|---|---|
| `box3d` | Alpha. You will read it, patch it, and update it often |
| `bc7enc_rdo` | Not in Conan Center. Two files |

### Excluded on purpose

| Rejected | Reason |
|---|---|
| DirectX Shader Compiler | Not packaged, and the build is difficult. shaderc with GLSL is enough. Revisit only for SM6.x features |
| assimp | Too large. Use cgltf and support glTF only |
| Boost.Asio as a job system | An I/O reactor, not a scheduler for frame parallelism. See §5.1 |
| Boost.Hana for reflection | Slow compiles and poor error messages. See §7 |
| protobuf | The wrong shape for game networking. Use reflection-driven bit packing |
| Box2D | The target is 3D only |
| An audio plugin ABI | Too early. One implementation exists and no second one is planned |

#### 5.1 Threading model

enkiTS owns the worker pool and all frame-parallel work. The Box3D task callbacks use it,
so physics and game logic share one scheduler instead of competing for cores.

Asio was considered for this role and rejected. It is a completion-handler I/O reactor. It
has no work stealing and no cheap fork-join. Game parallelism is almost always "split this
across N cores, then join before the sync point." If async network or streaming I/O is
needed later, Asio or a small dedicated I/O thread can serve that need next to enkiTS. One
scheduler still owns the cores.

---

## 6. Repository layout

```
engine/
  cmake/               Conan integration, compiler flags, shader compile rules
  profiles/            Conan profiles (linux-clang, windows-msvc, linux-clang-asan)
  conanfile.py         Options: with_editor, with_ui, with_lua, with_audio
  third_party/         box3d, bc7enc_rdo (submodules and thin CMakeLists)
  src/
    core/              logging, assert, arena and pool allocators, handles,
                       time, jobs (enkiTS wrapper), Tracy macros
    math/              glm wrapper, transform, AABB, frustum, conventions.h
    reflect/           field descriptors, attributes, type registry, and the
                       two consumers: the ImGui inspector and JSON
    platform/          SDL3 window, input, filesystem, dynamic library loading
    gfx/               PUBLIC render interface: handles, descs, command list
      vulkan/          the ONLY place vulkan.h is legal (rule 4.1)
    render/            render graph, PBR passes, materials, culling, content/
    scene/             EnTT world, transform hierarchy, serialization, prefabs
    physics/           box3d integration, fixed-step loop, debug draw
    script/            sol2 bindings, ScriptComponent, hot reload
    assets/            runtime asset DB, handles, streaming, hot reload
    ui/                moth_ui IRenderer/IImage/IFont implementations, see §8
    audio/             IAudioDevice and the miniaudio implementation
  tools/
    cooker/            source assets to cooked assets, separate executable
  apps/
    editor/            engine_core with the ImGui editor
    runtime/           engine_core, loads a project and runs it
  tests/
  sandbox/             the small game you build next to the engine
```

---

## 7. Reflection

This system gives the largest return in the engine. You describe each type once. The ImGui
inspector, JSON serialization, Lua binding, prefab overrides, and any later network
replication all read that one description.

**Approach.** Start with a hand-written `describe<T>()` that returns a tuple of field
descriptors. That is about 200 lines that you own in full. If the macro ergonomics
disappoint you, fall back to `boost::describe`. Boost.Hana is rejected for slow compiles
and poor diagnostics. Later, a libclang codegen step over annotated headers removes the
macros. Design the descriptor format so that this move does not change any consumer.

**Give each field an extensible attribute list from day one.** Adding this later is very
costly. The minimum set is:

- `Range{min, max, step}` — slider bounds
- `Tooltip`, `Category` — editor presentation
- `Hidden` — reflected, but not shown
- `ReadOnly` — shown, but not editable
- `Transient` — serialization skips it
- `Version` — schema migration
- `EditorOnly` — removed from shipping builds

**Validation rule.** Never ship a reflection design with one consumer. Build the ImGui
inspector and the JSON serializer together in M2. A single consumer never proves the
abstraction.

**What M2 built.** `reflect/reflect.h` holds the descriptors, `reflect/inspector.h` holds
the ImGui consumer, and `reflect/json.h` holds the serialization consumer. Both consumers
call only `for_each_field()`, `field_count()`, and `type_name()`, so the later libclang
step can replace the descriptors without touching either one.

The two consumers split the attribute list cleanly, which is the result the validation rule
was looking for. `Range`, `Tooltip`, `Category`, `Hidden`, and `ReadOnly` reach only the
inspector. `Transient` and `Version` reach only the serializer. No attribute needed a change
to serve the second consumer, and no consumer needed a field the other one added.

`Version` drives the schema migration. The version of a type is the largest `Version` on any
of its fields, and the writer stores it under `__version`. A field that the document
predates keeps its default and the reader stays quiet. A field missing from a document that
should carry it is a warning, because that points at a truncated file.

**What the third consumer needed.** Scene files arrived in M3.2, and they were the first
consumer nobody designed the descriptors alongside. The descriptor format needed no change.
`for_each_field()` served the component writer and the component reader as they were.

One gap did appear, and it is not in the descriptors. A scene file names its components,
and C++ cannot build a type from a string. `reflect::Registry` stores a name, a size, and a
field count, and none of that lets a reader act on a type. `scene::ComponentRegistry` closes
the gap by storing function pointers that already know the type.

That leaves two registries where the design wants one. Folding the operations into
`reflect::Registry` is the answer, and it waits for a third caller to say what the set of
operations should be.

**What prefab overrides needed.** M3.3 added the fourth consumer, and it needed nothing from
the descriptors either. A prefab override has to name one field, and it gets that granularity
for free: `to_json()` writes one key for each described field, so a patch that names one key
overrides one field.

The override is an RFC 7386 merge patch over the component set of one entity. The prefab
supplies the defaults, the patch supplies the changes, and a field the patch does not name
still comes from the prefab. That one line of ordering is the whole of "editing a prefab
reaches every instance that left the field alone". `scene::override_patch()` walks two
documents and keeps only what differs, so an instance stores the field it moved and nothing
else.

A merge patch reads a null as "remove this key", so an instance can drop a whole component.
No described field writes a null, so nothing collides with that meaning.

The gap here is structural, not per-field. An instance cannot record a child you added, a
member you destroyed, or a member you reparented. Issue #27 tracks that. No content file can
reach it, because a scene file gives an index only to the records it writes and a prefab
instance is one record. Only code can build that shape, so the trigger is a tool that edits
a live world structurally.

**What the editor needed.** M3.3 closed with a window that edits any component on any
entity, and it named no component type. `scene::ComponentOps` gained an `inspect` pointer
next to `save` and `load`, and it calls the same `reflect::inspect()` the M2 window calls.
So the first consumer of the descriptors is also the last one to arrive, this time reached
through a name in a registry rather than through a type in the source.

That is the third operation on the pile in `scene::ComponentRegistry`, and it sharpens
issue #25 rather than settling it. The set is now save, load, and inspect, which is enough
of a shape to fold into `reflect::Registry` when a fourth caller asks.

**What the asset identity needed.** M4.1 put a `Guid` in a described field, and that was the
first field type neither consumer could carry. Both held a closed list: a described type, a
glm vector, a quaternion, a list, or a plain scalar. A GUID is none of those. Describing it
as two 64-bit numbers would have compiled. It would also have written a nested object of two
large integers. A person reading a diff needs one string instead.

`reflect::TextValue` in `reflect/traits.h` answers it. A type declares `to_text` and
`from_text` next to itself, and argument-dependent lookup finds them. The serializer then
writes a string, and the inspector draws a text box that commits when the user leaves the
field. The reflection layer names no such type, so `core/` can define one without `reflect/`
depending on a layer above it.

Note what did not change. `Describe<T>` is the same, every attribute is the same, and both
consumers still call only `for_each_field()`. This is not a change to the descriptors. It is
a change to what a field may hold. Keep the two apart. The libclang step in the first
paragraph of this section has to replace the descriptors without touching the field types.

**What the cooker needed.** M4.2 added the manifest, and it needed nothing new either. A
`ManifestEntry` is an ordinary described struct, and `std::vector<ManifestEntry>` reached the
serializer through the list branch that M2 already had. The `Guid` field went through the
`TextValue` branch M4.1 added, so a manifest reads as paths and identities rather than as
pairs of large integers.

**What the texture settings needed.** M4.3 put a `ColorSpace` enum in the sidecar and needed
nothing new. The enum branch would have written `"color_space": 1`, which says nothing to the
person who opens that file to fix a texture that came out wrong. So `ColorSpace` declares
`to_text` and `from_text` and reaches the same `TextValue` branch, and the sidecar reads
`"Linear"`. `TextureImport` itself is an ordinary described struct nested in `AssetMeta`, and
it carries `Version{2}`, so a sidecar written before M4.3 reads back with no warning.

That is seven consumers on one description, and the descriptor format has changed once. The
one change was a new kind of field value, not a new kind of descriptor.

The inspector needed one thing the other consumers did not. A value that must parse cannot
be written back on each keystroke, because a half-typed GUID is not a GUID. So
`widget::edit_text_value()` reports a change when the user leaves the field, and text the
reader rejects leaves the value alone.

---

## 8. Game UI — moth_ui

[moth_ui](https://github.com/instinkt900/moth_ui) is an existing C++17 library. It has a
retained-mode node graph, JSON layout files, Flash-style keyframe animation with
per-property tracks and more than 30 easing curves, and animation events. It also has a
visual authoring tool called `moth_editor`. It is already a Conan 2 package. It is already
backend-agnostic. A Vulkan backend exists in `moth_graphics` that you can reuse.

ImGui is the editor UI and the debug overlay only. It is not game UI, and it never will be.

### 8.1 Integration shape

Implement three interfaces in `src/ui/`. Write them against `gfx::`, never against Vulkan.

- `IRenderer` — the drawing surface
- `IImage` and `IImageFactory` — backed by engine texture handles
- `IFont` and `IFontFactory` — see §8.3

**Write `IRenderer` as a batching recorder, not as direct draw calls.** Use the same model
as ImGui. Collect quads into vertex and index buffers, then flush them in as few draws as
possible. The push/pop state stack maps onto batch breaks:

| moth_ui state | Implementation |
|---|---|
| `PushTransform` | Per-draw push constant. It replaces instead of composes, so a plain mat4 works |
| `PushClip` | Scissor rect. Forces a batch break |
| `PushBlendMode` | Pipeline variant. Forces a batch break |
| `PushColor` | Vertex color or push constant. No break needed |
| `PushTextureFilter` | Sampler selection. Batch break on change |

The primitives to serve are `RenderRect`, `RenderFilledRect`, `RenderGradientRect`,
`RenderImage` with all scale types, and `RenderText`.

### 8.2 Why this is worth doing

The costly half of game UI already exists and works. That half is the retained node graph,
the serialized layouts, the keyframe animation, and the authoring tool. Widgets are the
cheap half, and you add them one at a time. The build integration is close to free, because
the engine already uses Conan 2.

There is a second gain. moth_ui is a real external consumer of `gfx::`. Nothing else in the
engine tests whether that interface is truly free of Vulkan and ready for a C ABI, because
the same person writes both sides. The M5.5 spike in §10 exists to get that signal early,
while the interface still costs little to change.

### 8.3 Known costs

**The engine owns text rendering.** `RenderText` gives both layout and rasterization to the
backend. So you must write glyph rasterization with stb_truetype, or FreeType if you need
hinting. You must also write atlas packing and eviction, line breaking, and alignment. Add
HarfBuzz if you ever need CJK or Arabic. People underestimate this work every time. Check
what `moth_graphics` already implements before you write any of it.

**Controller navigation is architecture, not a widget feature.** A focus graph, directional
resolution, focus wrapping, and mouse/pad input-mode switching belong in the node tree. They
do not belong on individual widgets. Design this early if gamepad support matters. Adding it
later costs much more.

**The widget set is small.** Today it holds `widget`, `ui_button`, and `ui_scroll_view`.
Text input, sliders, toggles, dropdowns, virtualized lists, tabs, and modals are all
incremental work. Rule 4.6 applies. Add each one when `sandbox/` needs it.

### 8.4 Engine-side integration points

- **Layouts as cooked assets.** moth_ui layouts are JSON. The asset pipeline in M4 gives you
  GUIDs, dependency tracking, and hot reload. Make `.mothui` a cooked asset type. Rewrite
  image references from file paths to asset GUIDs. This makes moth_ui part of the engine
  instead of an external addition, and layout hot reload comes at no extra cost.
- **Input bridge.** Translate SDL3 events into moth_ui events. Controller navigation will
  live at this seam.
- **Lua bindings.** Bind moth_ui nodes through the reflection system. This gives you menus
  driven by script, which is the right way to author UI behavior.

### 8.5 Boundaries

- Keep it an optional dependency behind the `with_ui` Conan option. `engine_core` must not
  depend on it. This costs nothing and keeps a replacement possible.
- Leave `moth_editor` as a standalone tool for now. It works. You can fold it into the
  engine editor later, because both use ImGui. It is not a milestone.
- Keep moth_ui in its own repository with its own release cadence. Consume it by version
  pin. Do not vendor it.

**One caution.** An engine plus a UI library is two projects, and the engine alone takes
years. This is acceptable only because moth_ui already exists and works. The extra cost is
integration plus a few widgets. If you start a UI-library refactor while the renderer is
half finished, you have inverted the priority. Rule 4.6 applies with full force here.

---

## 9. Cross-cutting notes

**Time model.** Use a fixed timestep for physics and game logic, and interpolate for
rendering. Decide this now. Scripting, determinism, record and replay, and any later
networking all depend on it. Box3D supports deterministic replay if the simulation stays
deterministic.

**Memory.** Use a per-frame linear arena that resets each tick, plus pool allocators for hot
component types. Do not write a full custom allocator stack. These two save more time than
they cost. The rest do not.

**Scene graph.** EnTT has no parenting. The transform hierarchy is engine code: parent and
child links, local and world transforms, dirty propagation, correct update order, and
reparenting with no visual jump. It is more complex than it looks.

**Materials.** A material is a shader plus a reflected parameter block. So §7 gives you the
material editor at no extra cost. Choose graph-authored or code-authored before M5. That
choice changes everything after it. The default assumption is code-authored, with a possible
graph layer later.

**Shader pipeline.** Compile GLSL to SPIR-V offline with shaderc. Derive descriptor set
layouts with SPIRV-Reflect. Cache permutations. Support hot reload.

M4.2 moved this out of CMake and into the cooker. A shader is an asset now, with a `.meta`
sidecar and a manifest entry, and `assets::Content` reads it at startup.

The cooker invokes `glslc` as a separate program rather than linking `libshaderc`. Conan 2
has no per-target requirement. Linking it would therefore put shaderc in the graph of every
consumer of the package, for the sake of one tool. Issue #43 holds the reasons to change
that. The first one to arrive is M5, where permutations turn one process for each shader
into many.

The trade is the error text. glslc writes it to stderr and the cooker passes it through, so
nothing can read a file and a line out of it yet.

The cooked SPIR-V is a file rather than a header the build generates. `engine_core` therefore
has no build-time dependency on the cooker, and a shader reloads through the same path every
other asset uses.

**Textures.** M4.3 made a texture the second asset type on the cooker. `src/assets/texture.h`
holds the file format, and both sides read that one header. A cooked texture is a 32-byte
header and then every mip level, largest first, packed with no padding. The runtime reads the
file into one buffer and hands the payload to `gfx::create_texture`. There is no decode step
and no second copy.

The `.meta` sidecar decides three things: the color space, whether to compress, and whether
to build mips. The cooker guesses the color space once, from the file name, and writes it
into the new sidecar. After that the file decides. A guess that keeps overwriting what a
person typed is worse than no guess at all.

**The color space is the reason any of this is a cook step.** A mip chain has to average
light, not the bytes that encode it. Two black texels and two white ones are half the light,
and half the light writes back as 188 in sRGB, not as 128. A chain built the naive way gets
darker at every level, distant surfaces go muddy, and the cause is very hard to find. So the
cooker converts sRGB color channels to linear, averages, and converts back. Alpha never goes
through the transfer function, because alpha is coverage and it is already linear.

BC7 comes from `bc7enc_rdo`, which §5 vendors as a submodule. Only `bc7enc.cpp` is compiled.
An image narrower or shorter than one 4 by 4 block stays uncompressed, because it would be
mostly padding. `TextureImport::compress` turns it off for a lookup table, where an exact
texel matters more than the memory does.

`gfx::TextureFormat` carries the four combinations of RGBA8 or BC7 with sRGB or Unorm. The
sRGB entries make the sampler convert on read, which is what §3 asks for. Rule 4.2 holds:
the enum is a plain `uint32_t` and `TextureDesc` stays POD.

**Meshes, and the sub-asset problem.** M4.4 made glTF the third asset type, and it forced a
change the first two did not need. One glTF file holds several meshes and several materials.
A prefab has to name one mesh, so a cooked file holding all of them could not be referenced.

The manifest therefore maps one source to many outputs. `ManifestEntry` gained a list of
`ManifestOutput`, and each output carries the identity that a scene, a prefab, or another
asset stores. A rule that writes one file gives its output the source asset's own GUID, so
nothing about a texture or a shader changed.

**A sub-asset has no sidecar, so its GUID is derived rather than stored.** `Guid::derive`
folds the parent GUID, a kind word, and an index with the FNV-1a in `core/hash.h`. The same
three inputs give the same answer on every machine and on every run, and nothing has to stay
in step. The result carries UUID version 8, which RFC 9562 reserves for a custom scheme, so
it is a real UUID and it can never collide with the version 4 that `Guid::generate` returns.

The cost is that the index is positional. Reordering the meshes inside a source file moves
which part holds which index, and every reference to the moved part then points at another
one. A file a person edits keeps its order. An exporter that reorders is the reason to store
a name instead, and that change would move the format version.

**A glTF buffer is payload, not an asset.** A `.gltf` keeps its geometry in a `.bin` beside
it. That file is an input, so the manifest hashes it and editing the geometry cooks the mesh
again. It is also not an asset: the copy rule would put the vertex data in the cooked tree a
second time, where nothing reads it. So the cooker reads every glTF before it cooks anything,
and skips the files they name. A texture a glTF names is not like this. That one is a real
asset with a sidecar of its own, and the texture rule cooks it.

The vertices are interleaved rather than one stream for each attribute. One stream suits the
forward pass the engine draws today, and it costs one bind rather than four. A depth prepass
or a shadow pass reads position alone and would rather have it separate, so M5 is the
milestone that may split this.

**Drawing what the scene names.** M4.4c added `scene::MeshRenderer`, which names a mesh by
GUID, and `render::MeshPass`, which draws every entity that carries one. `render::MeshCache`
turns a GUID into a pair of GPU buffers and uploads each mesh once however many entities
name it. The render caches own the GPU buffers rather than a central asset database,
because a GPU buffer needs the device to free it and a central database would need to
reach into the device from the wrong layer. Each cache therefore holds its own GUID map
and its own fallback, and each cache grows a `drop()` for hot reload. M4.1 built an
`AssetPool` and an `AssetDatabase` with generational handles, but no production code
ever called it. The caches do the same job with fewer layers, so the database was
removed in #61 rather than kept as a second implementation of the same contract.
**The node tree becomes a prefab.** A glTF node tree is a scene fragment with one root and
parents that come first, which is the shape M3.3 already reads. So the importer writes a
prefab and a scene instances it, and there is no second hierarchy format. Every node becomes
an entity with a Name and a Transform, and a node with a mesh also carries a MeshRenderer.
The components go through the reflection descriptors, so rule 4.5 holds.

This is what makes a model usable without hand-writing an identity. Before it, a scene named
each cooked mesh itself, and those identities are derived rather than chosen, so a person had
to cook once and copy them out. The sandbox scene held six such lines for one model. It now
holds one prefab instance.

A prefab has exactly one root and a glTF scene may list several, so the cooker adds a root
when it has to. The Flight Helmet lists six. The added root sits at the identity transform,
so it moves nothing, and it gives an instance one entity to place.

The sandbox finds that prefab by source path rather than by identity, through the manifest.
The path is what a person edits and the identity is what the path became, so nothing in the
game names a GUID.

**`CubePass` is gone.** Every entity that draws now goes through `MeshPass`, which is what
made the pass worth removing rather than keeping as a fallback. The crates became a
`crate.gltf` in the game content tree, so the shape they draw is a cooked asset like anything
else. The engine content tree holds only the two mesh shaders now.

A hand-authored file still names a derived identity: `crate.prefab` holds the GUID of the
cube mesh. A cooked prefab does not, because the cooker derives it. That gap is the reason
authored content wants a way to name an asset by path, and it is not solved here.

**Materials.** M4.4b made the material the fourth asset type. A cooked material is one fixed
size header and no payload, because a material is a handful of numbers and a list of
references. It names its textures by GUID, and the glTF names them by URI, so the importer
reads the sidecar of each image and stores the identity from there.

That makes an image sidecar an input of the glTF file. Replacing a sidecar gives the image a
new identity, and a material that still stored the old one would point at nothing. The image
itself is not an input, because editing the pixels changes the texture and not the material.

It also moves the color space guess. Two rules can now reach an image first, and whichever
one gets there writes the sidecar. A sidecar the glTF rule wrote with the defaults would say
sRGB, and every normal map in that model would read as color from then on. So the guess
belongs to `image_meta()` in the texture rule, and both callers go through it.

**The format carries more than the renderer reads.** A cooked material holds the whole glTF
metallic-roughness set: five texture GUIDs, both color factors, the metallic, the roughness,
the normal scale, the occlusion strength, the alpha mode, the cutoff, and the double sided
flag. The pass binds the base color and shades it under the same key light and hemisphere
that M4.4c used.

That is deliberate. glTF hands the rest over for free, and a field added later would move the
format version and cook every model again. Rule 4.6 governs systems, and this is a data
format. M5 is the milestone that shades with the rest, and it is also where the double sided
flag starts to matter, because honoring it needs a second pipeline or a dynamic cull state.

A material lives on the submesh rather than on `MeshRenderer`. One mesh can use several, and
a single field on the component could not say which submesh got which. A per-entity override
belongs with the editor work in M8.

**An image with no file is a sub-asset too.** A glTF names its images three ways: a file
beside it, a buffer view inside a `.glb`, or a data URI. Only the first has a file, and only a
file can carry a `.meta` sidecar. The other two get a derived GUID under the kind word
`texture`, the same mechanism a mesh and a material already use, and the cooker writes the
cooked texture beside them.

That matters for the M4 done-when test. A `.glb` is the form most exporters produce by
default, so a person dropping one into `content/` is the common case rather than the odd one.
Leaving those images with no identity gave geometry and no textures.

**The color space of such an image comes from the material slot, not from a file name.** There
is no file name to read. A base color or an emissive map holds color, and a normal,
metallic-roughness, or occlusion map holds numbers, so the slot says which. That is a better
answer than the heuristic gives for a file, because it reads what the glTF states rather than
what somebody chose to call a file. An image used in both kinds of slot is a broken model, and
color wins: reading color as linear washes it out everywhere, which is the failure a person
notices.

Nothing can override these settings, because there is no sidecar to hold an override. Making
one would mean writing a file back into the source tree beside an asset that has none.

**The manifest records which cooker wrote it.** A rule that starts writing a new kind of
output changes nothing the freshness check looks at. The check compares identities, input
names, and input bytes, and a new output touches none of them, so an old manifest stays fresh
forever and the new output never appears. A person meets that as content missing after an
engine update, with no message and no failing build, and cooking into a clean directory fixes
it without ever saying what was wrong.

So `Manifest` carries a cooker version, and a tree an older cooker wrote cooks again in full.
The field defaults to zero rather than to the current version, because zero is what a
manifest written before the field existed reads back as. Defaulting it to the current version
would make every old manifest claim to be current, which is the failure the field exists to
catch. `save_manifest` stamps it, so no caller can forget.

**Hot reload closes the loop.** M4.5 is the milestone goal, because the four parts before it
give a cooker you have to run by hand. A person edits a source file, and the running program
shows the result. Three pieces do it, and each one is useful on its own.

`platform::DirectoryWatcher` walks the source tree on a timer and reports what moved. It
polls rather than asking the operating system, so one implementation serves both platforms
and a test drives it with no event plumbing. The cost is the walk, which suits a tree of the
size `sandbox/` carries. Issue #57 puts a native backend behind the same interface when a
tree grows past it.

A change is never reported on the walk that first sees it. An editor that saves by writing a
temporary file and renaming it over the original shows up as several changes in a few
milliseconds, and a large file is readable long before it is complete. Handing either one to
the cooker gives a parse error that names nothing the person did.

`platform::run_process` starts the cooker and waits. DESIGN.md section 6 keeps the cooker a
separate executable, so the runtime asks for the work rather than linking the importer, and
no cgltf or stb reaches a shipping build. The arguments go across as a list and no shell ever
sees them, which is what makes an asset path holding `$name` or a backtick ordinary rather
than dangerous.

`assets::HotReload` joins the two and reads the manifest again. `Content::reload` compares
the new manifest against the old one and names only the identities that moved. The entry hash
covers every input, so an asset the cooker skipped keeps the hash it had, and a reload after
one save names one asset rather than the whole tree.

**A failed cook changes nothing.** The cooker writes its manifest even when one asset failed,
so a half-cooked tree really does reach the disk. `HotReload` reads the exit code and refuses
that tree, which leaves the program with the assets it already had. The next save tries
again, and nothing here ends the process. A broken scene file is the same shape: the world
goes empty, the log names the file, and saving a working one loads it again.

**Freeing a resource waits for the frames in flight.** `MeshPass::reload` waits for the
device before it frees a buffer or a texture. A frame the GPU has not finished may still read
one, and that use-after-free is a failure the validation layer may or may not report, on a
run that may or may not reproduce. A reload follows a person saving a file, so the wait costs
a stall nobody sees. Streaming cannot pay that, and it brings a queue that frees behind the
frames instead, and issue #60 holds it.

A material holds the texture handle it resolved, so dropping a texture also drops every
material that named it. Keeping the material would bind a handle the device already freed.

**Looking at what was drawn.** `gfx::capture_frame` copies the frame that was presented last
into host memory, and `runtime --screenshot <file>` writes it as a PNG. A run that ends with
no error says the commands were valid. It says nothing about geometry that came out
mirrored, inside out, or upside down, and a renderer with no way to look at its own output
can only be checked by a person with the window open. The capture waits for the device to go
idle, so it is for the end of a run and not for every frame.

meshoptimizer runs two passes, and the order matters. The vertex cache pass reorders the
indices inside each submesh so a triangle reuses a vertex the GPU still holds. The vertex
fetch pass then reorders the vertices so the ones a triangle reads sit near each other.
Running fetch first would undo it.

**Profiling.** Add Tracy in M0. It integrates quickly and it changes how you work for the
rest of the project.

**Networking.** You are not building it. But three cheap decisions keep it possible later,
and all three are good design anyway. Use a fixed timestep. Sample input into a plain
serializable struct. Make world state serializable through reflection. With those, you can
add GameNetworkingSockets or ENet later instead of rewriting.

---

## 10. Milestones

Each milestone ends with something you can run.

**This section stays canonical.** It holds each definition and each "done when" test. The
GitHub issue tracker holds the state. Every milestone here has a GitHub Milestone, and the
work inside it becomes issues that link back to this section. Do not copy a definition into
an issue, because two copies drift.

### M0 — Foundations
CMake, Conan 2, and profiles. An SDL3 window. Logging, asserts, and a live Tracy
connection. enkiTS running a `parallel_for`. Math and `conventions.h`. A frame arena
allocator. CI with the rule 4.1 grep.
**Done when:** a blank window opens, the profiler shows a live graph, and Tracy shows jobs.

### M1 — Vulkan bring-up
volk and VMA. Device selection. A swapchain that handles resize correctly. Frames in
flight. Dynamic rendering. Clean validation layers. Draw a triangle, then a textured mesh
with an orbit camera. You define the `gfx::` handles and descriptor structs here.
**Done when:** a cube spins, validation reports no errors, and resize works.

### M2 — Reflection
Field descriptors with the attribute list from §7. Build two consumers together: the ImGui
inspector generator and the JSON serializer.
**Done when:** you register any struct, edit it live in ImGui, and round-trip it to disk.

### M3 — ECS and scene
EnTT. A transform hierarchy with dirty propagation and correct update order. Scene
serialization through M2. Prefab instancing with per-instance overrides.
**Done when:** you load a `.scene` file, fly through it, and edit entities live.

`sandbox/` starts here, as a library rather than an application. Rule 4.3 says the editor
and the runtime are two applications over one core, and the game links into both. Building
the game as a library from the start is what keeps that true, five milestones before M8
depends on it. The runtime links `camina::sandbox` today.

### M4 — Asset pipeline
**This milestone decides whether the engine is usable.** Asset GUIDs. The `cooker`
executable. glTF import with cgltf. Textures through stb and bc7enc_rdo with mip chains.
meshoptimizer. A manifest with dependency tracking. File-watch hot reload.
**Done when:** you copy a `.gltf` file into `content/` and it appears in the running editor.

### M5 — PBR and render graph
A frame graph that handles barriers and transient resource aliasing. Cook-Torrance
metallic-roughness. IBL: an HDR environment converted to irradiance SH, prefiltered
specular, and a BRDF LUT. Cascaded shadow maps. ACES tonemap. Materials as a shader plus a
reflected parameter block.
**Done when:** a Sponza-class scene renders correctly.

### M5.5 — moth_ui spike, 2 to 3 days, timeboxed
Write a minimal `IRenderer`, `IImage`, and `IFont` against `gfx::`. Render one static layout
with an image and a string. **The purpose is diagnostic, not feature work.** It tells you
whether an external consumer can use `gfx::`, while the interface still costs little to
change. Fix what the spike exposes, then stop.
**Done when:** one moth_ui layout draws in the engine, and you have written down what the
spike taught you about `gfx::`.

### M6 — Physics
Connect Box3D to enkiTS. Add rigid body and collider components. Reflect them, so the
inspector needs no extra work. Add debug draw. Add a fixed timestep with render
interpolation.
**Done when:** you hit a stack of boxes and it falls.

### M7 — Scripting
sol2. A `ScriptComponent` with `on_start`, `on_update`, and `on_destroy`. Reflection-driven
binding with a **curated** surface. A fully mechanical binding produces an API that nobody
enjoys. Add hot reload from the start.
**Done when:** the sandbox game logic runs entirely in Lua.

### M8 — Editor split
`editor` and `runtime` as separate executables over `engine_core`. The game module links
into both, so the editor holds the project types and can inspect them. A release build
compiles with `WITH_EDITOR` off and drops the editor code. See rule 4.3.

Play-in-editor through world snapshot and restore, which M2 and M3 already provide.
ImGuizmo. An asset browser, a hierarchy panel, and an inspector panel.
**Done when:** you build a level in the editor, press play, and ship it as a runtime build.

### M9 — Game UI
Complete the moth_ui integration on the M5.5 foundation. Add the batching recorder, the font
atlas and text rendering, layouts as cooked assets with hot reload, the SDL3 input bridge,
and the Lua bindings. Add widgets when `sandbox/` needs them.
**Done when:** the sandbox game has a main menu, a pause menu, and a HUD. You author them in
`moth_editor` and they hot-reload.

### M10 — Audio
miniaudio behind `IAudioDevice`. Positional 3D and buses.
**Done when:** the sandbox game plays sound.

### After that, as the game demands
ozz-animation, which you pull forward as soon as you need a character. The `gfx::` plugin
ABI. Controller navigation for moth_ui. Networking.

---

## 11. Sequencing rules

**Do M4 before M5.** You will want to build the PBR renderer first. It gives fast visible
results. But a renderer with no asset pipeline can draw only programmer cubes. The pipeline
decides whether anyone can make a game with this engine, including you in a year.

**Start `sandbox/` at M3.** Choose something small and specific. A physics puzzle game fits
well. It exercises Box3D, scripting, and PBR, and it needs no animation and little UI.

**Keep M5.5 timeboxed.** If it starts to become M9, stop. Its value is the interface
feedback, not the pixels.

---

## 12. Risks

| Risk | Mitigation |
|---|---|
| You build the renderer and never finish the asset pipeline | The §11 sequencing rule. Start `sandbox/` at M3 |
| `gfx::` hardens into a Vulkan-shaped interface | Rules 4.1 and 4.2 with CI enforcement. The M5.5 external-consumer spike |
| Box3D changes under you during alpha | A pinned commit, a vendored submodule, and a thin engine-side wrapper |
| Text rendering costs more than planned | Called out in §8.3. Audit `moth_graphics` before you write any of it |
| moth_ui turns into a competing project | Rule 4.6. Add widgets only when the sandbox needs them. See the §8.5 caution |
| Reflection needs attributes added later | Design the attribute list in M2, before any consumer ships |
| Conventions drift | Write §3 down and commit it before the first triangle |

---

## 13. Open questions

1. Does Box3D use +Y up? Check its default gravity vector and heightfield orientation in the
   samples. See §3.
2. Are materials code-authored or graph-authored? Decide before M5. See §9.
3. What does the `moth_graphics` Vulkan backend already implement for font atlasing and text
   layout, and how much can you reuse? See §8.3.
4. Does `moth_editor` become a panel in the engine editor, or stay standalone? Not urgent.
   Revisit after M8. See §8.5.
5. Should moth_ui drop `fmt` and `range-v3` for `std::format` and `std::ranges`? This matters
   only if moth_ui moves to C++20. It is cosmetic, and it removes two transitive
   dependencies.
