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
| `miniaudio` | latest | Audio, M11 |
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

**An instance records shape as well as fields.** A merge patch changes a field and nothing
else, so on its own it cannot say that you added a child, destroyed a member, or moved one.
Each of those used to be lost on the next save, and an added child went with a warning as its
only trace.

The instance record therefore carries three more keys next to the overrides: the members it
destroyed, the entities it added, and the members it moved. They share one index space with
the prefab. A prefab of N entities owns 0 to N-1, and the entities the instance added
continue from N, so a parent index reads the same whichever kind it names.

That shared space is what lets a member move under an entity the instance added. It also
means an instance builds in two steps rather than one: every entity is created, then every
entity is attached. The prefab format guarantees a parent comes before its child, and that
guarantee covers the prefab's own entities and nothing the instance did to them.

Destroying an entity in a world takes its subtree, so a record written from a live world
lists every one. A record somebody edited may name a parent and leave a child behind, and
building that child would attach it to an entity that is not there. So the removal closes
over the tree on the way in, to a fixed point rather than in one pass, because a moved member
can name a parent with a higher index.

A member dragged out of the instance entirely reads as destroyed. From the root there is no
longer any way to tell the two apart, and inventing one would need a second link back.

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
the same person writes both sides. The M6 spike in §10 exists to get that signal early,
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
material editor at no extra cost.

**Materials are code-authored.** A person writes GLSL, and the parameter block comes from
SPIRV-Reflect rather than from a hand-written descriptor. This was §13 open question 2, and
M5 needed the answer, because the choice changes everything after it.

Code-authored wins on cost. A graph needs a graph format, a node editor, and a shader
generator, and the node editor belongs to the editor milestone rather than to the renderer.
Nothing here blocks a graph layer later. A graph would write GLSL, so it sits on top of this
rather than replacing it.

**Shader pipeline.** Compile GLSL to SPIR-V offline with shaderc. Derive descriptor set
layouts with SPIRV-Reflect. Cache permutations. Support hot reload.

M4.2 moved this out of CMake and into the cooker. A shader is an asset now, with a `.meta`
sidecar and a manifest entry, and `assets::Content` reads it at startup.

M5.1 added the reflection, and it runs at cook time. The cooker links SPIRV-Reflect, reads
the module glslc wrote, and stores the descriptor layout and the members of every uniform
block beside it. `src/assets/shader.h` holds that format. The runtime reads it and never
links SPIRV-Reflect, so the shipping binary carries no reflection.

The module and its description travel in one file rather than two. Two files would need two
manifest entries and a derived GUID for the second, and they could drift apart. One file
cannot describe a module it does not carry.

**A shader cooks once for each variant its sidecar lists.** The name carries the part number,
so `mesh.frag` gives `mesh.frag.0.shader`. The sidecar holds a `shader` block with a list of
variants, and each names its defines. An empty list, which is what every sidecar written
before this holds, cooks one module with no defines.

The variants are written rather than worked out. A cross product of every toggle grows by
powers of two, and most of those combinations no material ever asks for. `assets::ShaderImport`
in `src/assets/meta.h` holds the list.

The first variant is the base form and it must define nothing, because it keeps the identity of
the source asset. A reference to the shader itself therefore still resolves. Every other
variant derives an identity under the kind word `shader`, the same way a mesh inside a glTF
file does.

Each cooked module records the defines it was built with. A consumer picks a variant by what it
declares, and not by where it sits in the manifest, so reordering the list moves no meaning.

**`mesh.frag` is the first shader with variants, and `render::MeshPass` selects one for each
submesh.** The shader compiles out two things: the normal map and the occlusion map. That is
four forms. Every other map is read with no branch, because a slot the material left empty
binds a white texel that costs the same to sample. `render::mesh_variant_index` turns the maps
a material named into the form it needs.

Every form declares the same descriptors. A sampler stays declared even in the form that never
reads it, because a declaration inside an `#ifdef` would give each form a different set layout,
and one material descriptor set could then not bind against another form. Vulkan calls that
undefined rather than an error, so it appears as a wrong texture. `MeshPass::build_pipelines`
compares each form against the base form and refuses the set when they disagree.

The pass builds all eight pipelines at start, four forms for the opaque draws and four for the
blended ones. A hot reload rebuilds all of them together, or keeps all of the old ones. Half a
scene from a new shader and half from an old one is worse than a stale picture.

The opaque draws are not yet grouped by the form they need, so a mixed scene rebinds more than
it has to. `MeshPass::pipeline_switch_count` measures it and issue #105 holds the work, which
belongs with the render graph in M5.3.

`gfx::GraphicsPipelineDesc` now takes the bindings rather than a `sample_texture` flag, so no
layout is written by hand anywhere. That closes the gap rule 4.5 names: a hand-written
pipeline layout beside a reflected parameter block is two descriptor systems.

The cooker no longer passes `-O` to glslc. glslc hands `-O` to spirv-opt, which strips every
`OpName`. The set and the binding survive, because they are decorations the module needs to
be correct, and every name does not. A parameter block with no names gives §7 nothing to
label a field with. Issue #90 holds the measurement and the two ways to keep both.

The cooker invokes `glslc` as a separate program rather than linking `libshaderc`. Conan 2
has no per-target requirement. Linking it would therefore put shaderc in the graph of every
consumer of the package, for the sake of one tool. Issue #43 holds the reasons to change
that, and it was re-read when permutations arrived. One of the four triggers it names expired
when `platform::run_process` replaced `std::system`, because a define is a vector entry now and
there is nothing left to quote. The permutation trigger is real and it needs the cook measured
rather than guessed, which belongs with #90.

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

**Authored content names an asset by path, and the cooker resolves it.** A cooked sub-asset
has a derived identity and nobody chooses it, so a person cannot know it until the cooker has
run once. Content the cooker writes is fine, because it derives both ends itself. Content a
person writes was not: `crate.prefab` held the GUID of the cube mesh, copied out of the
manifest by hand.

So a scene and a prefab may write `asset:models/crate/crate.gltf#mesh:0` where a GUID goes,
and the cooker turns that into the identity before it writes the file. The cooked document
still holds only GUIDs. The path is the authored form and the GUID is the cooked one.

**A save writes the authored form, not the resolved one.** A live world holds identities,
because that is what the engine reads. A document a person edits again holds references. So
saving walks the document and puts each reference back, which is the mirror of what the cooker
does on the way in. `assets::reference_for` finds the reference that names an identity, by
deriving from the source identity until it matches.

That is why the syntax lives in `src/assets/reference.h` rather than in the cooker. Both ends
need it, and a second copy would drift.

The scene a person edits is in the source tree, so that is where a save goes. Writing to the
cooked tree looks like it worked, and the next cook throws it away. A build with no source
tree beside it cannot save at all, and says so rather than writing somewhere useless.

**The rename cost moves rather than disappearing.** A GUID exists because it survives a
rename, and an authored path does not. Renaming a file inside the content tree breaks every
authored reference to it, and each one has to be edited. Nothing at runtime is affected,
because the cooked document holds identities, and the next cook fails and names each
reference that no longer resolves. That is the trade: a path a person can read and type,
against an edit when a file moves. A reference resolved once at load time rather than at cook
time would make the same trade and pay it on every run.

Resolution reads the sidecar of the named file and derives from there, so it needs nothing
cooked first and the order of the tree does not matter. A file with no sidecar yet gets one,
because a scene can be reached before the model it names.

This makes a wrong reference loud. Before it, a wrong GUID drew nothing and reported one line,
which looks exactly like a mesh that failed to upload. Now the cook fails and names the
document, the path, and what is wrong with it.

Deriving an identity is what makes that hard to get right. `Guid::derive` answers for any
index, so `#mesh:7` on a file holding one mesh gives a GUID that looks like every other one
and names nothing. Only the finished manifest can tell the two apart, so the cooker checks
every reference against it after the whole tree is cooked. That check also covers a document
the cooker skipped, because a model that loses a mesh breaks a reference nobody touched.

A reference names a file inside the content tree, so an absolute path and a `..` step are
both refused. Resolving one would read a file the tree does not own, and writing its sidecar
would put a file next to it. A cook runs on a build machine over content that arrives from
somewhere else, so that is a refusal rather than a warning.

A scene and a prefab stopped being copied through to get this, which means the cooker parses
both. A file that will not parse now fails the cook rather than reaching the runtime and
emptying the world there. The cooked document is written back out rather than copied, so it is
normalized JSON and not byte for byte what the source held.

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

**The format carried more than the renderer read, until M5.2.** A cooked material holds the
whole glTF metallic-roughness set: five texture GUIDs, both color factors, the metallic, the
roughness, the normal scale, the occlusion strength, the alpha mode, the cutoff, and the
double sided flag.

M4.4b wrote all of it and the pass bound the base color alone. That was deliberate. glTF hands
the rest over for free, and a field added later would move the format version and cook every
model again. Rule 4.6 governs systems, and this is a data format.

M5.2 reads it. The shading is Cook-Torrance metallic-roughness, and every map and every factor
reaches the shader. A material binds one descriptor set that names all five textures and a
64-byte block of factors, which is `render::MaterialUniforms`. A slot the material left empty
binds the fallback white texel, and a bit in that block says which slots were really named, so
a normal map that is not there does not tilt every normal the same way.

Every alpha mode works. Mask discards below the cutoff, which the shader does on its own.
Blend needs the pipeline as well, so `MeshPass` holds a second one that blends by source alpha
and does not write depth. A blended submesh does not draw where the view hands it over. It
waits, and the pass sorts every one of them back to front and draws them after the last opaque
surface.

Sorting is per object, because a fragment cannot see the one behind it. That is the known
limit of blending in a forward pass, and a blended mesh that overlaps itself still draws in
index order. Order-independent transparency is the fix and no milestone asks for it yet.

A material lives on the submesh rather than on `MeshRenderer`. One mesh can use several, and
a single field on the component could not say which submesh got which. A per-entity override
belongs with the editor work in M9.

**Lighting scales by clustered forward, not by deferred shading.** The frame block carried a
fixed array of eight lights until M5.7a. That followed rule 4.6, because the sandbox lights a
scene with two. The M5 done-when test is a Sponza-class scene, which carries far more, so the
milestone needed an answer rather than a larger array.

M5.7a took the first two of the three steps. The list is a storage buffer that grows to fit, so
the count is a number rather than a constant. A point light whose range sphere misses the camera
frustum never reaches the buffer. That carries a few hundred lights.

M5.7b is the third step and it carries thousands. A compute pass divides the frustum into a grid
of tiles across depth slices, and writes a short list of lights for each cell. The forward pass
reads the list for the cell a pixel is in, so the cost follows the lights near a pixel rather
than the lights in the scene. Measured over a room of 1024 point lights with 837 in view, the
mesh pass falls from 100.7 ms to 11.2 ms and the picture does not change. The cull itself costs
0.5 ms.

The slices grow exponentially in view distance rather than linearly in depth. Reverse-Z puts
the near plane at 1 and infinity at 0, so a linear split of that range packs fifteen of sixteen
slices inside the first 1.6 metres and leaves the whole room in the last one. The last slice
also reaches past where the grid stops, because a fragment beyond that distance clamps into it
and a slice that ended there would leave a far light out of the list.

A cell held a fixed 256 light indices and dropped the rest with no message. That number was
measured. At 64 the scene above lost light on 36 percent of the frame. At 256 it matched a
shader that loops over every light exactly. It moved the cliff rather than removing it, which
was issue #175.

**The per-cell capacity follows the light count now, so a cell drops nothing.** It doubles from
256 up to 2048 and the grid grows with it. Both shaders read the number out of a uniform block
they already bind, so nothing recompiles. A cell that holds every visible light cannot drop one,
whatever the camera does. So the guarantee is structural and no measurement has to confirm it.

The old cap was silently wrong long before 2048. Take a room of 513 point lights of 8 m range.
Capping a cell at 256 there moves 44.6 percent of the frame. The fitted capacity is 1024, and the
worst pixel is off by 213 of 255 on a channel. The capacity buys that back, and it costs what the light costs.
The mesh pass goes from 59.1 ms to 85.5 ms, because those lights are now shaded rather than
dropped. The cull itself pays 0.07 ms of it. In the sandbox nothing changes at all, because three
lights fit either way and the pass measures 1.452 ms on both sides.

Two things cover a scene past 2048. The host warns, and the frame report says whether a drop is
possible at all rather than counting drops after the fact. And the light list is then ordered by
luminance times range. So a crowded cell keeps the lights that put the most light into the scene
rather than the ones the loop reached first. That ordering cuts the mean error of the capped
frame above by 36 percent, from 1.690 to 1.084 of 255. It is one order for every cell and not a
choice for each. A dim lamp beside a cell can still lose to a bright one far away.

`--cluster-cell-lights` lowers the ceiling. No sandbox scene reaches the drop path on its own
now, so measuring the loss needed a way to force it.

Removing the ceiling needs a compacted index list, where a count pass gives each cell an offset
into a list sized for the scene. That trades a second pass over the lights for memory that fits,
and nothing reaches the ceiling yet.

Deferred shading answers the same question and it was rejected. It was the answer when no
compute shader could cull lights up front, and it carries four costs that clustered forward
does not. It writes a G-buffer, which is a large bandwidth bill at high resolution. It gives up
hardware MSAA. It needs a second forward path for the transparent surfaces, so the engine keeps
two shading paths that must agree. And it holds every material to the fields the G-buffer
carries, which fights the reflected parameter block above.

The order matters. The cull pass is a compute pass that writes a resource the mesh pass reads,
which is exactly what the frame graph in M5.3 exists to schedule. So the light grid landed after
the graph and not before it, and the cull declares its write the way every other pass declares
one. The barrier between the dispatch and the fragment reads falls out of `derive_barriers`
rather than being written by hand. Issue #98 holds the decision.

The grid is the first graph resource that is a buffer rather than an image, and the derivation
needed nothing new for it. A `ResourceId` is only an index and the states are the whole
vocabulary, so the caller that issues the barriers is the only part that has to know a buffer
has no layout to change.

It is also the first buffer that only the GPU touches. A uniform or a storage buffer is
host-visible and mapped by default, which is right for a block the CPU rewrites every frame and
wrong for three megabytes one shader fills and another reads. `BufferDesc::device_only` is the
answer, and `update_buffer` refuses such a buffer rather than writing through a pointer that is
not there.

**A mesh is culled against the camera frustum before it is drawn, and that belongs to M5.** The
light cull answered this question for lights and nothing answered it for geometry. `MeshPass` used
to walk every entity that named a mesh, whatever the camera was pointing at, and issue a draw for
each submesh. That was correct and it did not scale.

It belongs to M5 rather than later because the M5 done-when test is a Sponza-class scene. That
scene is 3.75M triangles in 405 primitives, and issue #130 names the missing cull as the reason
it would not be interactive on the reference GPU. A done-when test that cannot run is not a test.

M5.7c does it. `src/math/bounds.h` turns the local bounds of a mesh and a world matrix into a
world-space sphere, and `MeshPass::draw` tests that sphere against the planes `cull()` already
extracted for the lights. One extraction serves both, so the two cannot disagree about which
camera the frame belongs to. An entity that misses issues no draw, and `culled_mesh_count()`
reports what went.

The test is per entity and not per submesh. The bounds of the whole mesh decide whether any part
of it can be seen, and a mesh that is in view then draws every part. A tighter per-submesh test is
issue #177, and it needs a scene where one mesh is large enough for the difference to measure.

The radius is exact for the transformed box rather than an upper bound on it. The cheap form,
which scales the local half diagonal by the longest matrix column, looks safe and is not: three
columns that lean the same way have a signed sum longer than any one of them. An underestimate
drops a mesh that is on screen, and the hole appears at the edge of the frame where nobody is
looking for it. So four of the eight corner offsets settle the radius, and the other four are
those negated.

A sphere and not a box. The test was written and tested already, one test beats six
plane-versus-box comparisons, and a sphere that a scaled matrix produced is conservative in the
direction that keeps a visible object on screen. A box would cull more and it can wait for a case
that measures the difference, which rule 4.6 says to wait for.

The cull runs on the CPU, one test for each entity. GPU-side culling and indirect draws answer a
larger question, which is a scene whose draw list is too large for the CPU to walk at all. Nothing
in M5 asks that, and the answer would be wasted before the draw list is built from something other
than an EnTT view.

The sandbox proves the correctness and it cannot measure much of a saving. Every entity is in view
from the camera the sandbox opens with, so the cull drops nothing and the picture is byte for byte
what it was. Turning the camera 45 degrees drops 5 of 27 entities, and that picture is byte for
byte identical to the same view with no cull. That is the result that matters: a conservative test
must never make a hole. Turning it 180 degrees drops 22. Five draws survive, which are the walls of
the room, because the camera stands inside them and their spheres therefore contain it.

The saving in this scene is small, and saying so is the point. Against the same camera the mesh
pass goes from 1.536 ms to 1.510 ms at 45 degrees, and from 0.094 ms to 0.002 ms at 180. The first
is inside the run-to-run spread. A GPU already rejects an off-screen triangle cheaply, so culling a
handful of small meshes on the CPU buys almost nothing. What it buys is the draw call and the
vertex work, and the sandbox has too little of either to show it. That is a reason to expect the
cull to pay at Sponza scale rather than a reason to doubt it, and #88 is where it gets measured.

Comparing two different cameras is the mistake to avoid here. The mesh pass costs 1.46 ms looking
at the room and 0.002 ms looking away. Reading that pair as the effect of the cull overstates it by
a factor of fifteen. Almost all of that difference is the camera.

The 180 degree measurement exposes the next piece of work. The shadow pass stays at 0.36 ms on
that view, so it is now the whole cost of a frame that draws almost nothing. It cannot reuse this
frustum, because a mesh behind the camera still casts into a cascade. The test has to run against
each cascade in turn instead.

**M5.7d does that.** `ShadowPass::draw` extracts the six planes of each cascade. It tests every
entity against them. A mesh that misses one cascade issues no draw into that layer, and it may
still draw into another. So the counts are for each entity and cascade pair. One entity is tested
four times.

The test cannot change the map. That is the argument for it, rather than a hope about it. Nothing
enables depth clamp anywhere in `gfx::`, so the rasterizer already clips what falls outside those
six planes. The cull drops the draw for geometry the GPU was going to throw away.

It pays better than the camera cull does. Two reasons, and both are about the shadow pass rather
than about the test. First, the pass runs four times over the same world, so one culled entity
saves up to four draws. Second, a cascade volume covers a slice of the camera frustum rather than
the whole of it. So even the opening view leaves most of the room outside most of the cascades.
Against the same camera:

| Camera | shadow draws of 92 | culled | no cull | cull |
|---|---|---|---|---|
| As the sandbox opens | 65 | 27 | 0.452 ms | 0.391 ms |
| Turned 45 degrees | 64 | 28 | 0.421 ms | 0.369 ms |
| Turned 180 degrees | 41 | 51 | 0.363 ms | 0.249 ms |

The picture is byte for byte identical at the first two cameras, which is where the shadows are on
screen to be compared.

A cascade volume is orthographic, and that is where both depth planes are real. An infinite
perspective has a degenerate far plane. So every earlier test of `frustum_from_view_projection`
drove the one case that cannot see a wrong depth pair. `tests/test_frustum.cpp` drives an
orthographic reverse-Z volume as well now. One of its checks fails if a perspective matrix takes
the place of the orthographic one. An orthographic volume must not narrow with distance.

**The graph derives barriers first, and aliases memory when something needs it.** A full
aliasing allocator is a large piece of work, and today there is one pass and nothing to alias
against. So M5.3 builds the half that is already owed: real stage and access masks derived from
what each pass declared, and `ALL_COMMANDS` gone from the swapchain transition. That shortcut is
correct and slow, which was right for M1 and wrong for a milestone whose test is a Sponza-class
scene.

Aliasing gets a seam rather than an implementation. The graph knows every resource lifetime
already, because it knows the reads and the writes, so the allocator is an addition and not a
rewrite. Rule 4.6 says to build it when two transients are live and neither needs the other.

That second condition is not met yet, and the shadow map and the scene color are why. They are
both live, but the mesh pass reads the shadow map and writes the scene color in the same pass, so
their lifetimes touch and neither can take the other's memory. A depth prepass target or a second
shadow cascade atlas would be the first genuinely disjoint pair. Issue #122 holds the work and
names the wrong pair, which is worth fixing before somebody starts from it.

**begin_frame hands over an image in no state at all, and the graph moves it.** The frame used
to transition its own color target and its own depth target. That put the barriers in the one
place that cannot know what the first pass wants. Both images now start in
`ResourceState::Undefined`, and the graph issues what it derived.

Presentation stays in `end_frame`. That wait belongs to the present semaphore, and a caller who
forgot the barrier would present an image in the wrong layout.

**Undefined says the contents are worthless, not that the image is idle.** Those are two
different claims. Taking the first for the second is a real race.

A source stage of `TOP_OF_PIPE` with no access orders a transition against nothing. Two hazards
follow. A swapchain image is still being read by the acquire, and one depth image is shared by
every frame in flight. So a transition out of `Undefined` waits on the stage the new state uses.

Synchronization validation reports both. It is off by default, because it costs real time on
every frame.

**A pass declares its reads and writes as data, not through calls into a builder.** A pass
returns a descriptor naming what it reads, what it writes, and in what format. The graph is then
a pure function of that data.

That is the whole reason for the choice. A builder that a pass calls during a setup phase reads
more naturally when the declaration is conditional, and it is the shape most published frame
graphs use. But it cannot be tested without standing up a graph, and a graph cannot be stood up
without a device. Issue #62 records that the render caches have no tests for exactly that reason,
and putting the barrier logic behind the same wall would repeat the mistake at the point where it
costs most. Barrier derivation is where a renderer hides bugs that appear on one vendor and no
other, so it is the part that most needs a test with no device in it.

**The environment is a cubemap an entity names, and it arrives before the lighting that reads
it.** The cooker turns an equirectangular `.hdr` panorama into six faces with a mip chain, and
`scene::Environment` names the result by GUID. One frame binds one cubemap, so the first entity
that carries the component wins.

The component sits on an entity rather than on the world. A prefab can then carry an
environment, and the inspector edits it the way it edits every other component. The transform
of that entity means nothing, because an environment has no place.

M5.4a binds it and samples it directly, which is deliberately not image based lighting. The
split sum approximation wants an irradiance term and a prefiltered term with a lookup beside
them, and one texture read stands in for each. A rough metal therefore reads too sharp. That
seam is the point: the transport is provable on its own, and the filtering that follows is a
change to the shader rather than to the format. M5.4b made that change, and it moved no format.

Two texture shapes now reach one shader, and they are not interchangeable. A `sampler2D` and a
`samplerCube` need different fallbacks, because a descriptor a scene left unfilled still has to
bind something valid. So `render::TextureCache` holds a white texel and six grey texels, and it
refuses a cooked file whose face count does not match the binding that asked for it. Vulkan
calls the mismatch undefined rather than an error.

**Image based lighting is split in two, and the cooker writes both halves.** The split sum
approximation separates what a surface receives into a diffuse term and a specular term, and
the two want different things from the same panorama.

The diffuse term is irradiance, and it is nine coefficients of a second order spherical
harmonic. Irradiance over a hemisphere is a very smooth function of the normal, so nine numbers
carry it to within a percent or two, and the whole diffuse environment then costs no texture
read at all. `src/assets/irradiance.h` holds the format, and it stores the coefficients with the
per band convolution already folded in, so a shader evaluates plain polynomials and keeps no
table of constants that could drift from the cooker's.

The specular term is the mip chain of the environment cubemap, filtered by roughness rather than
by a box. Level 0 is the environment as it is, which a mirror reflects, and each level below it
holds the GGX lobe for a rougher surface. So the chain is not a resampling of the level above
it: every level is importance sampled from the panorama, because filtering an already filtered
level would blur twice.

**The irradiance is a sub-asset and the cubemap keeps the source identity.** A scene names one
environment, and the second part derives its GUID from the first under the kind word
`irradiance`, which is the mechanism a glTF already uses for its meshes and materials. Putting
the coefficients inside the cooked texture file was rejected, because `src/assets/texture.h` is
the format every 2D texture shares and environment-only data there would move its version for
assets that gained nothing.

**The BRDF table hangs off a source file that carries no data.** The third part of the split sum
depends on no environment, no material, and no scene. It is the same numbers in every project,
so cooking it once for each environment that shares it would be waste. It gets
`src/render/content/ibl.brdf`, whose sidecar holds its size and its ray budget and whose body
holds only a note saying why the file exists.

That is odd to look at and it is still the cheapest answer. The alternative was a well known
GUID written into the code, and that would make it the one asset whose identity does not come
from a sidecar, with its size and sample count as C++ constants rather than something a person
can retune and cook. A source file with no source data buys the whole existing model: a path, an
identity, a manifest entry, freshness, and hot reload.

**Two invariants pin the table, and they catch different errors.** A surface cannot reflect more
than reaches it, so the scale and the bias never add past one. And at no roughness the lobe is a
mirror, so they add to exactly one at every angle. The second says nothing about shadowing,
because a mirror has none to do, so only the first catches a Smith term that stops shadowing at
a grazing angle. The table reached eight at its worst entry while the mirror check still passed.

The Smith remapping differs between direct light and this. Direct takes `(roughness + 1) squared
over eight` and image based lighting takes `alpha over two`, where alpha is roughness squared.
The two look alike enough that using one for the other survives a reading.

**The shader reads the three parts and the pass binds them once for the frame.** The
coefficients ride in the frame block, because they are nine numbers that change when the scene
names another environment and on no other frame. The cubemap and the table are set 0 beside it.
A material set therefore carries only what a material owns, and a draw call binds no part of the
environment.

The diffuse term divides by pi, and that is the only place the divide happens. The cooked
coefficients carry the cosine convolution and nothing else, so a constant sky of radiance L
gives irradiance pi times L, and the Lambert term takes it back to L. Splitting the divide
between the cooker and the shader would leave two files that must agree about half a constant.

**A scene with no environment gets the irradiance of the one it is actually given.** The grey
cubemap fallback is a constant environment, so its irradiance is pi times that grey in every
direction, and the first coefficient carries all of it. That is a computed answer rather than a
chosen one, and it keeps the diffuse and the specular of such a scene coming from one number.
`render::kFallbackCubeTexel` holds the number so the two cannot drift.

**The sandbox ships a row of metal spheres across the roughness range.** Every other model in it
is one material at one roughness, and image based lighting is exactly the difference between a
smooth metal and a rough one. Without the row a prefiltered chain filtered the wrong way reads
as a picture that is a little dull, with nothing pointing at the cause. With it, the failure is
a row that does not soften from left to right.

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

**A shader cannot swap behind a handle, so it rebuilds the pipeline.** A mesh and a texture
arrive by GUID and the cache hands out a new one, and nothing above notices. The SPIR-V is
built into a `gfx::PipelineHandle`, so a changed shader means a new pipeline.
`MeshPass::reload_shaders` builds one into a fresh handle, and only swaps once it holds. A
shader that will not build leaves the pipeline that is drawing alone, because somebody
editing a shader breaks it often and losing the picture on every typo would make the loop
useless.

This is why the runtime watches two trees. `sandbox/content` holds what an artist edits and
`src/render/content` holds the shaders, which are the assets a programmer edits. The engine
tree holds only the two shaders, so any change to it rebuilds the pipeline.

**A cook that fails keeps the asset the tree already had.** The cooker rebuilds its manifest
from what cooked this run, so an asset that failed used to lose its entry. The cooked file
stays on disk, because a rule that fails writes no output, and the manifest then no longer
names it. The running program is unaffected, and the next start fails on an asset that is
sitting right there.

Nothing met that before shaders reloaded, because a shader that would not compile failed the
build instead. Editing one live makes the first typo reach it. So a failed source keeps the
entry the last cook gave it, and the log says so.

**A reload says what each asset was, not only that it moved.** An identity that went away is
not in the new manifest, so nothing can be looked up about it afterwards. Without the kind
recorded at the moment it went, a deleted prefab and a deleted texture read the same, and the
world built from that prefab stood until a restart. `assets::AssetChange` therefore carries
the cooked path and whether the asset is gone, taken from the manifest being replaced.

The rebuild that follows is the whole world and not the part that changed. Doing less means
knowing which entities came from which prefab, building only those, and holding a selection
across it. That is the editor's job and it arrives with the editor. Rebuilding everything
costs one scene load, which is what a person just asked for by saving. A mesh or a texture
still swaps in behind the entities that name it, so the common change touches no entity at
all and whatever was selected stays selected.

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
the game as a library from the start is what keeps that true, five milestones before M9
depends on it. The runtime links `camina::sandbox` today.

### M4 — Asset pipeline
**This milestone decides whether the engine is usable.** Asset GUIDs. The `cooker`
executable. glTF import with cgltf. Textures through stb and bc7enc_rdo with mip chains.
meshoptimizer. A manifest with dependency tracking. File-watch hot reload.
**Done when:** you copy a `.gltf` file into `content/` and it appears in the running program,
with no restart.

The test said "the running editor" while it was written, and M4 closed five milestones before
the editor exists. What M4 really built is the loop: the watcher sees the file, the cooker
runs, and the world builds again. The runtime is what runs that loop today, and the editor
will run the same one in M9. Naming the program rather than the application keeps the test
honest about what it measures.

### M5 — PBR and render graph
A frame graph that handles barriers and transient resource aliasing. Cook-Torrance
metallic-roughness. IBL: an HDR environment converted to irradiance SH, prefiltered
specular, and a BRDF LUT. A clustered light cull, which the graph schedules. Cascaded
shadow maps. ACES tonemap. Materials as a shader plus a reflected parameter block.
**Done when:** a Sponza-class scene renders correctly.

### M6 — moth_ui spike, 2 to 3 days, timeboxed
Write a minimal `IRenderer`, `IImage`, and `IFont` against `gfx::`. Render one static layout
with an image and a string. **The purpose is diagnostic, not feature work.** It tells you
whether an external consumer can use `gfx::`, while the interface still costs little to
change. Fix what the spike exposes, then stop.
**Done when:** one moth_ui layout draws in the engine, and you have written down what the
spike taught you about `gfx::`.

### M7 — Physics
Connect Box3D to enkiTS. Add rigid body and collider components. Reflect them, so the
inspector needs no extra work. Add debug draw. Add a fixed timestep with render
interpolation.
**Done when:** you hit a stack of boxes and it falls.

### M8 — Scripting
sol2. A `ScriptComponent` with `on_start`, `on_update`, and `on_destroy`. Reflection-driven
binding with a **curated** surface. A fully mechanical binding produces an API that nobody
enjoys. Add hot reload from the start.
**Done when:** the sandbox game logic runs entirely in Lua.

### M9 — Editor split
`editor` and `runtime` as separate executables over `engine_core`. The game module links
into both, so the editor holds the project types and can inspect them. A release build
compiles with `WITH_EDITOR` off and drops the editor code. See rule 4.3.

Play-in-editor through world snapshot and restore, which M2 and M3 already provide.
ImGuizmo. An asset browser, a hierarchy panel, and an inspector panel.
**Done when:** you build a level in the editor, press play, and ship it as a runtime build.

### M10 — Game UI
Complete the moth_ui integration on the M6 foundation. Add the batching recorder, the font
atlas and text rendering, layouts as cooked assets with hot reload, the SDL3 input bridge,
and the Lua bindings. Add widgets when `sandbox/` needs them.
**Done when:** the sandbox game has a main menu, a pause menu, and a HUD. You author them in
`moth_editor` and they hot-reload.

### M11 — Audio
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

**Keep M6 timeboxed.** If it starts to become M10, stop. Its value is the interface
feedback, not the pixels.

---

## 12. Risks

| Risk | Mitigation |
|---|---|
| You build the renderer and never finish the asset pipeline | The §11 sequencing rule. Start `sandbox/` at M3 |
| `gfx::` hardens into a Vulkan-shaped interface | Rules 4.1 and 4.2 with CI enforcement. The M6 external-consumer spike |
| Box3D changes under you during alpha | A pinned commit, a vendored submodule, and a thin engine-side wrapper |
| Text rendering costs more than planned | Called out in §8.3. Audit `moth_graphics` before you write any of it |
| moth_ui turns into a competing project | Rule 4.6. Add widgets only when the sandbox needs them. See the §8.5 caution |
| Reflection needs attributes added later | Design the attribute list in M2, before any consumer ships |
| Conventions drift | Write §3 down and commit it before the first triangle |

---

## 13. Open questions

1. Does Box3D use +Y up? Check its default gravity vector and heightfield orientation in the
   samples. See §3.
2. What does the `moth_graphics` Vulkan backend already implement for font atlasing and text
   layout, and how much can you reuse? See §8.3.
3. Does `moth_editor` become a panel in the engine editor, or stay standalone? Not urgent.
   Revisit after M9. See §8.5.
4. Should moth_ui drop `fmt` and `range-v3` for `std::format` and `std::ranges`? This matters
   only if moth_ui moves to C++20. It is cosmetic, and it removes two transitive
   dependencies.
