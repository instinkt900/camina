# Camina Engine — Design & Roadmap

Status: M0 complete
Last updated: 2026-08-02

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

**4.3 — The editor is an application, not a build mode.**
`engine_core` is a library. `editor` and `runtime` are two executables that link it. Use
`#ifdef EDITOR` only to remove editor-only reflection metadata from shipping builds. Never
use it to change engine logic.

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
    reflect/           field descriptors, attributes, type registry
    platform/          SDL3 window, input, filesystem, dynamic library loading
    gfx/               PUBLIC render interface: handles, descs, command list
      vulkan/          the ONLY place vulkan.h is legal (rule 4.1)
    render/            render graph, PBR passes, materials, culling, shaders/
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

**Profiling.** Add Tracy in M0. It integrates quickly and it changes how you work for the
rest of the project.

**Networking.** You are not building it. But three cheap decisions keep it possible later,
and all three are good design anyway. Use a fixed timestep. Sample input into a plain
serializable struct. Make world state serializable through reflection. With those, you can
add GameNetworkingSockets or ENet later instead of rewriting.

---

## 10. Milestones

Each milestone ends with something you can run.

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
`editor` and `runtime` as separate executables over `engine_core`. Play-in-editor through
world snapshot and restore, which M2 and M3 already provide. ImGuizmo. An asset browser, a
hierarchy panel, and an inspector panel.
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
