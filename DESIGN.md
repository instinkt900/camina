# Camina Engine — Design & Roadmap

Status: M0 through M13 complete. No milestone is open.
Last updated: 2026-08-22

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

**The pin is `3fc20f5b453ba9e14cdf54ecafa87a2a4bcdf53c`, taken on 2026-08-11.** That commit is
dated 2026-07-29 and it was the upstream head. Record the new commit and the date here when
you move the pin. The reason to move it is what makes the next update readable.

`third_party/CMakeLists.txt` builds it. It adds `box3d/src` and not the Box3D root, because
the root is written for a top-level build. The root sets the static MSVC runtime, and
`profiles/windows-msvc` asks Conan for the dynamic one. MSVC does not link objects that
disagree about the runtime. The root also carries the `-ffp-contract=off` that the solver
needs for determinism, so that flag moves to the library here. Issue #229 holds the part of
it that does not reach engine code.

**Only files under `src/physics/` include a Box3D header.** `engine_core` links `box3d` as a
private dependency, so the include directory reaches nowhere else.
`scripts/check-box3d-containment.sh` fails CI when a header escapes anyway. This is not rule
4.1, which protects a later ABI extraction. It protects the update path: an alpha we patch is
a diff against one directory, not against the whole engine.

**What it actually cost, now M7 has closed.** Less than the alpha label suggested. It was not
patched once. The whole of it is one flag moved out of the root CMakeLists, and the containment
script has never had anything to report.

Nine things it did cost, each of which had to be read out of the source rather than out of the
documentation. M7 found the first four, M8.4 found the next three, and the sanitizer gate found
the last two:

- **Gravity is −10, not −9.8.** The documentation says one and the code says the other. §3
  holds the check that pins it.
- **A sleeping body ignores a velocity, silently.** `b3GetBodyState` returns null for a body
  outside the awake set and `b3Body_SetLinearVelocity` then returns early. It wakes the body
  first, but only when the velocity is not zero. So a zero on a sleeping body does nothing at
  all.
- **The debug wireframe of a shape is cached by the application.** Box3D calls a
  `createDebugShape` callback the first time it draws a shape and hands that pointer back
  afterwards, so a host that does not register the pair on the world definition gets no shapes
  at all. It also culls the debug draw against a 100 metre box before it reports anything.
- **A hull stores half-edges**, so every edge is in the array twice and a wireframe that draws
  them all draws itself twice over.
- **A shape reports no event unless it asks for one.** `enableSensorEvents` and
  `enableContactEvents` are off by default, because the bookkeeping costs something for every
  shape in the world. Both belong on the shape that is **not** the sensor, which is what the
  Box3D comment on `b3Shape_EnableSensorEvents` says. A world whose sensors alone carry the
  flag reports nothing, and nothing says why.
- **A destroyed shape still arrives in an end event.** Box3D reports the end of an overlap when
  a shape goes away, so the id a reader gets can already be dead. `b3Shape_IsValid` is what
  that is for. A reader that skips the check reads the user data of a freed shape.
- **A sensor draws like any other shape, and Box3D colors it wheat.** A sensor gets a
  broadphase proxy the same as a solid shape, so `b3World_Draw` reaches it with no extra call,
  and `DrawQueryCallback` gives it `b3_colorWheat` rather than the solid color. So
  `--physics-debug` shows a trigger, and shows it apart from a collider, with no engine code.
  Neither half is documented, so `tests/test_physics.cpp` pins both.
- **A task name does not outlive the call that queues the task.** `b3Solve` formats the name
  into a stack buffer inside the block that queues the task, so the buffer is gone before a
  worker reads it. `jobs::enqueue` keeps its own copy for that reason, and `jobs.h` says so.
  It used to keep the caller's pointer, which is a read of a dead stack frame on every solve.
  Nothing reported it: a dead frame usually still holds the bytes. AddressSanitizer named it
  in five test binaries at once. See issue #453.
- **A debug colour is not always an enumerator.** `b3MakeDebugColor` packs a `b3DebugMaterial`
  into the high byte of a `b3HexColor` and casts the result back to the enum, so the debug draw
  hands over values such as `0x1A9A9A9`: `b3_colorDarkGray` with `b3_debugMaterialMatte` above
  it. Loading that through an enum-typed lvalue is undefined behaviour.
  `engine::physics::raw_color` copies the bytes instead. The picture never moved, because
  `from_box3d` masks each channel to eight bits and drops the material byte. See issue #456.

**The solver is deterministic across threads.** Three offscreen runs of the sandbox, with a
crate thrown at a stack on a fixed frame and the solver split over eight job system workers,
produce images that are equal byte for byte. `-ffp-contract=off` is what that rests on, which
is why the flag moved into `third_party/CMakeLists.txt` rather than being left in a root
nobody builds.

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

**Box3D agrees.** M7.1 checked it at the pinned commit, and all three answers point the same
way:

- `b3DefaultWorldDef()` in `src/types.c` returns a gravity of `{0, -10, 0}`, which is down
  −Y.
- `b3HeightFieldDef` counts its grid lines with `countX` and `countZ`, so a height field lies
  in the XZ plane and its heights run along Y.
- `docs/loose_ends.md` says "Box3D uses a right-handed coordinate system. Positive Y is up by
  default", and it asks for meters, kilograms, seconds and radians.

So nothing converts at the boundary. `physics::default_gravity()` reads that vector out of the
library rather than repeating it, and `tests/test_physics.cpp` checks the axis and the sign. A
Box3D update that turns the world over then fails a test instead of turning the game over.

Two smaller findings. The magnitude is 10 rather than 9.81, which is a game number. And the
Box3D documentation says −9.8 while its code says −10, so read the code.

---

**miniaudio agrees with these axes, and takes no conversion.** Its spatializer is right
handed with forward at −Z and it takes a world up vector, which is what the table above says
the engine is. So `audio::Mixer::set_listener` passes a pose straight through. A conversion
there would be the mirror this section exists to prevent, and a mirrored sound is heard
rather than seen: every picture stays correct while the left and the right ear swap.

`tests/test_mixer.cpp` measures it rather than trusting it. A sound at +X has to be louder in
the right channel, a sound at −X louder in the left, and a sound straight ahead even. Those
three fail together if either half of the agreement changes.


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

`WITH_EDITOR` may remove an editor-only method, member, or subsystem. It must never change
what the code that remains does. An `#ifdef` around a branch inside a function that both
builds run is a violation, because it lets a shipping build behave unlike the editor build.
That class of bug is miserable to trace, and this rule is the only thing that prevents it.

**A reflection attribute that drops a field is the same violation, and `EditorOnly` was
deleted for it.** The attribute was declared in M2 and read by nothing. Making it work means
the serializer skips a tagged field when `WITH_EDITOR` is off, so the editor writes a scene
the shipping build reads differently. That is a shipping build behaving unlike the editor
build, which is exactly what this rule forbids. No field ever carried the tag, so deleting it
cost nothing. Hold editor state in an editor type, not in a tagged field of a shipped one.
See issue #305.

The macro name matches the Conan option `with_editor` and matches Unreal, so one name
means one thing across the build, the package, and the code.

**4.4 — Vendor only what Conan cannot give you.**
A dependency goes in `third_party/` only when you may need to patch it, or when no package
manager entry can be used at all. Everything else comes from Conan. Today this rule covers
three entries.

"No entry that can be used" is stricter than "not on Conan Center". A recipe that exists and
pins a dependency this engine cannot take is the same problem: `imguizmo` on Conan Center
requires `imgui/1.90.5`, and the editor is on the docking branch of 1.92, so Conan refuses
the graph. See §5.

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
| `imguizmo` | Every Conan Center recipe pins `imgui/1.90.5`, and this engine is on `1.92.8-docking`. One source file, built behind `with_editor` |

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

M7.2 connected Box3D to it. `jobs::enqueue` and `jobs::wait` are what that needed, because
the solver starts several pieces of work and joins them itself, which `parallel_for` cannot
express. The two Box3D callbacks pass a plain function pointer straight through, so a task
costs no allocation. The pool holds 256 tasks, which is the most Box3D queues in one step,
and it says so in its own header for this exact reason.

**A task slot keeps its own copy of the name.** The name reaches the profiler on the worker
rather than on the caller, so a pointer is the wrong thing to keep: Box3D builds its names on
a stack frame that ends before the task runs. The slot holds 32 bytes and cuts a longer name,
which costs 8 KiB over the whole pool and no allocation. `jobs::task_name` is what makes the
copy checkable, because a test can only tell a copy from a pointer by overwriting the buffer
the caller passed.

**`jobs::worker_count()` counts the calling thread.** enkiTS creates one fewer thread than
it reports, because `parallel_for` and `wait` both run work on the caller. Box3D counts the
same way, so the number passes across with no adjustment. The old comment on that function
said the opposite and was wrong.

**The pool costs more than it saves below about 130 bodies.** Measured on the reference
machine with 8 workers, over 120 steps of a box stack on a floor:

| Bodies | On 8 workers | On 1 worker |
|---|---|---|
| 13 | 0.038 ms | 0.024 ms |
| 65 | 0.086 ms | 0.057 ms |
| 129 | 0.142 ms | 0.164 ms |
| 289 | 0.216 ms | 0.367 ms |
| 769 | 0.345 ms | 0.915 ms |

So the crossover is near 129 bodies, and the sandbox will sit below it. Threading stays on
by default anyway. The loss there is 0.014 ms in a 16.7 ms frame, which is under a tenth of
a percent, and the win above the crossover is 2.7 times. A body-count heuristic would buy
that tenth of a percent and cost a mode switch that changes which code path a bug lives in.
Pass a worker count of 1 to `physics::World` when a scene wants the cheaper side of it.

`tests/test_physics.cpp` prints both sides of the crossover on every run. It asserts on
neither, because a wall-time threshold on a shared machine fails for reasons that have
nothing to do with this code. It does assert that the two runs put the stack in the same
place, which is what catches a finish callback that returns early.

---

## 6. Repository layout

```
engine/
  cmake/               Conan integration, compiler flags, shader compile rules
  profiles/            Conan profiles (linux-clang, windows-msvc, and the two debug ones)
  conanfile.py         Options: with_editor, with_ui, with_lua, with_audio
  third_party/         box3d, bc7enc_rdo (submodules and thin CMakeLists)
  src/
    core/              logging, assert, arena and pool allocators, handles,
                       time, jobs (enkiTS wrapper), Tracy macros
    math/              glm wrapper, transform, AABB, frustum, conventions.h
    reflect/           field descriptors, attributes, type registry, and the
                       two consumers: the ImGui inspector and JSON
    editor/            the panels both applications draw, the view state they
                       edit, and play mode. Not behind WITH_EDITOR, see below
    platform/          SDL3 window, input, filesystem, dynamic library loading
    gfx/               PUBLIC render interface: handles, descs, command list
      vulkan/          the ONLY place vulkan.h is legal (rule 4.1)
    render/            render graph, PBR passes, materials, culling, content/
    scene/             EnTT world, transform hierarchy, serialization, prefabs,
                       the camera a scene plays through
    physics/           box3d integration, the solver, debug draw
    script/            sol2 bindings, ScriptComponent, hot reload
    play/              the fixed step a game runs on: the clock, the solver,
                       the scripts, and the input on that clock. See §9
    assets/            runtime asset DB, handles, streaming, hot reload
    ui/                moth_ui IRenderer/IImage/IFont implementations, see §8
    audio/             IAudioDevice and the miniaudio implementation
    import/            the import rules: source assets in, cooked assets out.
                       engine::import knows no game. The cooker and the editor
                       both link it, a runtime build does not. See §10 M13
  tools/
    cooker/            the command line over src/import/, separate executable
                       so a cook runs on a machine with no graphics driver
  apps/
    editor/            engine_core with the ImGui editor
    runtime/           engine_core, loads a project and runs it
  tests/
  sandbox/             the small game you build next to the engine
```

### `src/editor/` is not behind `WITH_EDITOR`

The panels are in `src/editor/` and they compile into `engine_core`, so both
applications link them. M9.2 put them there, and the reason is rule 4.3: the editor is
an application and not a build mode. The inspector has run as a debug overlay in the
runtime since M2, so a panel that only the editor could draw would take that overlay
away.

So `src/editor/` means "authoring and inspection, for whichever application asks", not
"code a release build drops". ImGui is already an unconditional dependency of
`engine_core` for the same reason, and `WITH_EDITOR` gates the `apps/editor/`
application alone.

Two things follow. A panel header names no ImGui type, the way `reflect/inspector.h`
does, so a program that never opens a window still compiles. And a panel places itself
nowhere: the runtime overlay calls `place_next_panel` to open its windows at fixed
places, and the editor lets its dockspace decide. Neither layout is written into the
panel.

`editor::ViewSettings` lives here as well. It is the camera, the exposure, and the
simulation rate of one view, which both applications need and which an offscreen run
with no window reads too. It sits beside the panel that edits it rather than in the
`main.cpp` of one application, which is where it was until M9.2.

**The editor builds a default layout on a first run.** Three panels that place
themselves nowhere all open at the same spot, and the last one drawn buries the rest.
The runtime answers that with fixed positions. The editor docks, so its answer is a
layout: the hierarchy and the view settings on the left, the inspector on the right,
and the viewport in the middle. It runs once. As soon as `imgui.ini` holds a node for
the dockspace, the layout a person arranged is the one they get.

`editor::Viewport` lives here too, beside the panel that shows it. It owns the image the
scene is tonemapped into and the binding ImGui draws it through, and it names no ImGui
type and no Vulkan type, the same way the panels do. §9 holds why the scene renders at
the size of the panel and why the rebuild waits for the top of a frame.

`editor::FlyCamera` is here as well, and it is the clearest case of what this directory
means. It is a free-fly camera: two angles, a point, and the keys that move them. It is
input state and never a camera in its own right. The runtime steers the scene camera with
it, and both applications fall back to it for a scene that carries no camera at all. So it
belongs to neither application and to no scene.

**The runtime draws through `scene::primary_camera` and the editor draws through its own
view.** M9.5b split them. The editor viewport always shows the editor camera, a running
session included, so a person can fly around a game while it plays through its own eye. The
saved pose goes beside `imgui.ini` under `platform::preferences_directory`, because where
somebody stands while they work belongs to them and not to the project.

`Describe<FlyCamera>` lists the pose and nothing else. How fast a person flies and how far
the mouse turns them are preferences of the application, so they stay in
`editor::ViewSettings` beside the panel that edits them. A reflected type lists what it
wants rather than every member it has.

**The asset browser is the only way to fill an asset field**, which is what M9.6 built. A
field marked `reflect::AssetRef` holds an identity, and an identity is the one thing §7 says
a person cannot know: the cooker derives it. So the inspector shows a name and takes no
typed text at all. A person drags a row out of the browser instead.

**The inspector adds and removes components, and the World panel deletes an entity.** The
M9.8 run found both missing on the first attempt: a prefab dropped into a level could never
be given a `RigidBody`, so it never fell, and nothing took an entity away again. Placing
things is not authoring without those two.

`ComponentOps` grew `create` and `remove`, filled from the one description like every other
operation, so a component the game defines appears in the add list with no editor change at
all. **A Transform cannot be removed**: every entity has one and the hierarchy reads it, and
`owns_transform` already marked it, so the rule needed no new flag.

Deleting takes the descendants, so the panel asks first and says how many go. M12 gave that
delete an undo, and the question stayed: it is the count that is worth a click, not the
finality. The Delete key asks nothing, because a key that stops to ask is a key nobody uses.

**A prefab dropped on the viewport becomes an instance where the pointer is.** That is the
other half of a browser: filling a field says what an entity uses, and a drop says an entity
exists at all. It lands where the ray through the pointer meets the ground, and a few metres
ahead of the camera when the pointer is on the sky, because a drop that goes nowhere is worse
than one in a reasonable place. The new instance is selected, so the gizmo is already on it.

`assets::prefab_name` moved out of `sandbox/game.h` for this. It is a cooker convention
rather than a game rule: it turns a cooked path into the name a scene file writes, and the
editor needs it to join a dropped identity to the prefab the library holds.

That makes one of the M9.6 rules structural rather than checked. An asset the cook has not
produced is not in the manifest, so it is not a row in the browser, so there is nothing to
drag. Nothing has to refuse it.

`reflect/` sits below `assets/` and cannot ask a manifest anything, so the name arrives
through `reflect::set_asset_namer`, which an application installs once. A program that
installs none shows the identity, which is what the inspector did before.

`editor::place_entity` in `editor/placement.h` is here too. It turns a world pose into
the local transform an entity stores, which is the arithmetic behind a gizmo drag, and it
names no gizmo library. That is what lets a test drive it with no window. The ImGuizmo
calls live in `apps/editor/gizmo.cpp`, because the editor application is the only program
that draws handles and the only target that links the library.

`editor::PlayMode` is here for the same reason. It is play-in-editor: a snapshot of the
world, a `play::Session` over it, and the restore that puts the authored scene back. M9.4
built it, and it needed no new mechanism, because `scene::save_scene` and
`scene::load_scene` already round-trip a world through a document. That is what §10 means
when it says M2 and M3 already provide it.

**Play and stop both replace every entity**, so anything holding one has to let go across
either line. EnTT hands the same numbers out again, which is why the applications drop the
selection at both ends and why the session is built by `play()` rather than held. A fresh
session is a script host with no instances, a clock at zero, and bodies read from the world
as it stands, so the second play is the same as the first.

**Stop drops the session before it reads the world back.** Writing a transform onto a
dynamic body does nothing, which is issue #284, so a restore that ran while the bodies were
alive would put every static thing back and quietly leave the dynamic ones where the game
left them. The session and every body it built are gone before the first entity is read.

**A running session takes the save button away.** The world under a session is a game part
way through a step, and writing that over the source scene would save the wreckage of a
play as the authored scene.

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

**What an enum needed.** M7.3 wanted a body type of static, dynamic or kinematic, which is
one axis with three values. Such a field drew as "unhandled" and wrote as a bare number, so
M7.3a described the enum itself. `Describe<E>` gains an `enumerators()` where a struct has a
`fields()`, and `ENGINE_ENUMERATOR` names each value once. The concepts do not overlap:
`Described` asks for `fields()` and `DescribedEnum` asks for `enumerators()`, so a consumer
that walks fields can never pick up an enum by accident.

The serializer writes the name and reads the name. A document that stores `2` for a body
type says something else the moment somebody inserts an enumerator above it, and nothing
warns. A value the description leaves out still writes its number, because losing it would
turn a description somebody forgot to update into a silent change of the data.

So the reader takes a number as well, and warns. It has to: a reader that refused one would
reject a document this writer produced, and `from_json` fails the whole object rather than
one field. An unknown *name* still fails, because a name is a deliberate spelling and a
wrong one is a mistake rather than a gap in the description.

**This is the second way to spell an enum, and that is on purpose for now.** `ColorSpace`
declares `to_text` and `from_text` and reaches the `TextValue` branch instead, which is why
a sidecar already reads `"Linear"`. The two are not interchangeable. `TextValue` is for a
value with an unbounded space that has to parse, like a GUID, and it draws a text box.
`DescribedEnum` is for a fixed set of names, and it draws a drop-down of exactly those names.
An enum should use the second one. `ColorSpace` predates it and is issue #235, because the
enumerator names and the `to_text` strings do not match today and changing them changes every
sidecar in the content tree.

---

## 8. Game UI — moth_ui

[moth_ui](https://github.com/instinkt900/moth_ui) is an existing C++17 library. It has a
retained-mode node graph, JSON layout files, Flash-style keyframe animation with
per-property tracks and more than 30 easing curves, and animation events. It also has a
visual authoring tool called `moth_editor`. It is already a Conan 2 package. It is already
backend-agnostic. A Vulkan backend exists in `moth_graphics`.

**We own moth_ui and moth_graphics.** Both are Conan editables on the development machine, so
an engine build picks up a library edit with no export step. This changes what an integration
problem is. A mismatch between the two can be fixed on either side, and the cheaper side is not
always the engine. Say which side each fix landed on.

**`moth_graphics` is a reference, not a dependency. Do not link it.** It requires
`vulkan-loader`, and the engine never links the loader because it uses volk. It also pulls SDL,
FreeType, and HarfBuzz. It is a second Vulkan backend, so linking it would put a parallel
renderer inside the rule 4.1 containment boundary. Read it and port from it.

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
| `PushTransform` | Applied to each corner as it is recorded. No break. See below |
| `PushClip` | Scissor rect. Forces a batch break |
| `PushBlendMode` | Pipeline variant. Forces a batch break |
| `PushColor` | Vertex color or push constant. No break needed |
| `PushTextureFilter` | Sampler selection. Batch break on change. Not built yet, see below |

The primitives to serve are `RenderRect`, `RenderFilledRect`, `RenderGradientRect`,
`RenderImage` with all scale types, and `RenderText`.

**The transform reaches the vertex on the CPU, not a push constant.** An earlier version of this
table said per-draw push constant. That breaks the batch at every node, which defeats the point
of a batching recorder. M6.2 measured it: eight transformed quads are one draw when the recorder
applies the matrix as it records, and eight draws when a push constant carries it.

So a batch breaks on a clip and on a blend mode, and never on a transform.

**A texture filter breaks the batch and nothing applies it.** M6.3 records the filter on each
batch and ends a run where it changes, so the recorder is honest. The bind is where it stops: a
`gfx::` sampler belongs to the texture it was uploaded with, so a filter cannot be chosen at draw
time. `UiPass` says so once in the log rather than drawing a nearest filtered image blurred and
reporting nothing. Issue #209 holds the `gfx::` work.

Only a change breaks the run, not the push itself. `NodeImage` pushes a filter around every image
it draws, so breaking on the push would give two images of one texture two draws for nothing.
`moth_ui::TextureFilter::Invalid` is the sentinel a layout that saved no filter loads, and it
keeps the filter already in force rather than counting as a change.

**A batch also ends where the texture changes**, because one draw reads one texture. A run of
rectangles, gradients or outlines carries a null texture and `UiPass` binds one white texel for
it. White is the identity of the multiply the fragment stage does, so a shape and an image take
one pipeline and one set layout rather than two of each.

**Each stack rule is different, and none of them matches its name.** `PushColor` composes with
the colour under it, `PushTransform` replaces, and `PushClip` intersects. Only `PushTransform`
documents that in moth_ui. Getting one wrong compiles and draws a wrong picture that reads as a
layout bug. A pop can also arrive with no matching push, so each stack keeps a default that a pop
cannot remove. See instinkt900/moth_ui#149.

**A colour leaves sRGB at the vertex.** The swapchain is `B8G8R8A8_SRGB` and the hardware encodes
on write, so an authored colour passed straight through gets encoded twice. Converting per vertex
rather than per fragment also makes a gradient interpolate in linear, which §3 asks for.

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

**The engine owns text rendering.** `moth_ui::IFont` declares no methods at all, and
`RenderText` gives both layout and rasterization to the backend. So the engine owns glyph
rasterization, atlas packing and eviction, line breaking, and alignment. People underestimate
this work every time.

**`moth_graphics` already solves it, so port rather than write.**
`src/graphics/vulkan/vulkan_font.cpp` is 441 lines that build a glyph atlas with **FreeType and
HarfBuzz**. It walks every codepoint in the face, packs the rects, uploads one RGBA atlas,
reads the underline position from the face metrics, and shapes with an `hb_buffer`.
`font.vert` and `font.frag` sit beside it. The atlas packing, the metrics, and the shaping are
backend-neutral, so the port is mostly the texture upload.

Take FreeType and HarfBuzz for that reason, and take them from the start. An earlier version of
this section suggested stb_truetype, with HarfBuzz added later only for CJK or Arabic. That is
the more expensive path now, because it means writing what the reference already has.

**M6.4 measured that choice rather than assuming it.** Only 13 lines of the 441 call HarfBuzz
and 15 call FreeType. The packer, the metrics, the measurement and the wrapping are neutral, so
the port costs about the same either way and the library choice buys shaping alone. stb_truetype
is already a dependency and would have cost nothing to add. It was still rejected, because its
shaping is partial: GPOS pair kerning only, with no ligatures and no complex scripts. M6 is a
diagnostic spike, and text that measures differently from `moth_editor` would make every layout
difference ambiguous. Matching the reference removes that variable.

`conanfile.py` pins the two versions `moth_graphics` pins, for the same reason. It also sets
`harfbuzz/*:with_glib=False`. The default pulls glib, and glib drags elfutils, gettext,
libiconv, pcre2 and libffi behind it. glib is also LGPL, which nothing else here is. With the
option off, `with_ui=True` adds exactly freetype, harfbuzz, libpng, brotli and bzip2.

**The atlas is RGBA8, white with the coverage in alpha.** That is what the reference does and it
is worth keeping. A coverage-only texture is a quarter of the memory and needs its own pipeline
or a shader permutation. White with coverage in alpha makes a glyph an ordinary textured quad,
so text draws through the same pipeline as an image and a plain shape, beside the white texel
of section 8.1. It is correct under an sRGB swapchain as well, because Vulkan applies the
transfer function to the color channels and leaves alpha linear.

**Two parts are a rewrite rather than a port.** The reference packs every glyph in the face.
That cannot work in general, because a CJK face carries more than 20000 glyphs and one atlas of
them is tens of megabytes. `engine::ui::Font` packs on demand instead, which closed issue #213.
U+0020 to U+00FF is a preload rather than a limit: it is about 190 glyphs in a 512 square atlas
at 32 pixels, and it is what a European interface needs on its first frame. Anything else packs
the first time shaping asks for it. Issue #214 still holds the cost of one atlas for each size.
The reference also walks the string once and backtracks its loop counter to the last break it
passed. Splitting the words out first says the same thing and cannot run an index past the
start of a line.

**A glyph is one frame late, and that is forced rather than chosen.** Growing the atlas repacks
everything, because the packer cannot relocate a rectangle it already placed, so every texture
coordinate moves. A frame part way through recording would then hold coordinates for an atlas
that the texture is not, and the strings it had already recorded would sample the wrong place.

So there are two atlases. `pack_glyph` grows the working one, and `glyph()` and `shape()` answer
for the uploaded one. `shape()` reports -1 for a glyph the texture does not hold and remembers
that somebody wanted it. `UiPass::refresh_fonts` runs after the draw, packs what was wanted,
replaces the texture, and parks the old one in the slot ring the vertex buffers already use, so
it is freed three frames later when nothing in flight names it. The frame after that draws the
glyph.

The cost is one frame of a missing letter, the first time a string uses a glyph nothing has used
before. Stalling the device instead, the way `MeshPass::reload` does, was the other option: it
trades a missing letter for a hitch on a frame nobody asked to pause.

**`borrow_texture` publishes the working atlas**, because borrowing says that the handle holds
the atlas as it stands. That is what lets `tests/test_ui_font.cpp` drive the whole one-frame
rule with no device.

`load()` is separate from `upload()` so that none of this needs a GPU to test. The
rasterization, the packing, the shaping, the measurement and the wrapping are all driven by
`tests/test_ui_font.cpp` with no device, the way `tests/test_frustum.cpp` drives the frustum.

**`RenderText` forces the alpha blend, and that is not optional.** A glyph is coverage in
alpha. moth_ui pushes no blend mode around a text node, and the recorder starts at
`BlendMode::Replace`, which takes the pipeline that does not blend. The coverage then stops
shaping the edge of a letter, and text draws thickened and hard edged. It stays readable, which
is what makes it dangerous: nothing reports it and only a screenshot shows it. The reference
backend forces the same mode. `RenderText` pushes and pops it, so the caller gets its own mode
back.

**A font name resolves the way an image path does.** `engine::ui::FontFactory` takes the
name-to-file map moth_ui expects, and the path in it is a source path that the cooked manifest
resolves. The cooker has no rule for a font file, so it copies one unchanged and the manifest
records it. Section 8.4 holds why the engine resolves the path rather than moth_ui carrying a
GUID.

The three editor-only methods on `IFontFactory` assert. `LoadProject`, `SaveProject` and
`GetCurrentProjectPath` serve `moth_editor`, which keeps its font list in a project file. A
runtime takes its fonts from the cooked tree, so reading a project file here would be a second
source of truth. **An interface that makes a runtime implement editor project files is a seam in
the wrong place, and #200 collects that.**

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
- **An asset identity, against a path. The engine resolves the path, and moth_ui does not
  change.** `IImageFactory::GetImage` takes a `std::filesystem::path`, and the engine names an
  asset by GUID. M6.3 settled this: `engine::ui::ImageFactory` treats the path as a source path
  relative to the game content root and looks it up in the cooked manifest, which is exactly the
  string `assets::Content::find` already takes.

  The alternative was to give moth_ui an identity the consumer defines. That is the better shape
  in the long run and it fits `moth_graphics` too, because a path is still a valid identity
  there. It was not taken now for two reasons. It changes `IImageFactory`, `NodeImage`,
  `LayoutEntityImage`, `moth_graphics` and `moth_editor` together, which is a wide change to make
  during a spike §10 says to keep timeboxed. And the engine side has to be written either way, so
  nothing about it is wasted.

  **What is given up is what a GUID is for.** A layout stores a path, so renaming a source file
  breaks the layout, and the engine solved that everywhere else at M4. The cooked layout type
  below is where it comes due: that rule rewrites an image reference, and it has to decide
  whether it writes a GUID into a `std::filesystem::path` or moth_ui learns a real identity type
  by then. Issue #211 holds it.

  **M10 made that choice, and moth_ui learns the identity type.** Writing a GUID as text into a
  `std::filesystem::path` works and reads badly, and it leaves the layout format saying one thing
  and meaning another. The count of real call sites is what settled it, and §10 M10 holds the
  count. A path stays a valid identity, so moth_graphics and any game reading loose files keep
  working.
- **M10.2 cooked the layout, and the authored form is moth_ui's own convention.** A layout
  names an image by a path relative to its own directory, because `moth_editor` writes one
  that way and `moth_packer` reads one that way. The engine follows the format rather than
  imposing a second convention on it, and `src/import/layout.cpp` translates: it joins the
  path to the layout's directory, resolves the sidecar, and writes the GUID. A path is the
  authored form and a GUID is the cooked one, which is the split `cook_document` already
  makes for a scene.

  The image sidecar becomes an input of the layout, so replacing an image identity re-cooks
  every layout that names it. The image itself is not an input: editing its pixels changes
  the texture and not the layout.

  `engine::ui::ImageFactory` reads both forms. A cooked layout gives it a GUID, and a caller
  inside the engine gives it a source path, because a person writing C++ cannot know a
  derived identity.

  **A rule that writes its output differently has to raise `assets::kCookerVersion`.** The
  freshness check compares identities, input names and input bytes, and none of those moved
  when this rule started writing a GUID. So an existing cooked tree stayed fresh and kept
  serving a layout full of paths. That was found by mutation testing rather than by a failing
  build, which is exactly the silent shape the constant exists to stop.
- **M10.3 loads a layout by identity, and a layout hot reloads.** `engine::ui::read_layout`
  takes the bytes from `assets::Content` rather than opening a file, so the same call serves
  the first load and every one after it. `moth_ui::Layout::Load` is not used, because it
  takes a path. The reading half opens no device, which is what lets `tests/test_ui_layout.cpp`
  drive it with no GPU.

  **A raw pointer to a node does not survive a reload.** `FrameContext` held
  `moth_ui::Node*`, taken once at start, and the reload replaced the owner and freed the old
  tree. The first frame after a save then segfaulted inside `Node::Draw`. The context points
  at the owner now and reads through it each frame. Nothing reported this: the picture was
  right, the reload logged success, and the crash came one frame later. Only running it found
  it, which is why the milestone test is a run rather than a compile.

  **A layout that will not parse keeps the one already drawing.** The cooker refuses it
  first, so the cooked tree keeps the last good bytes and the change list names nothing. The
  check in the runtime is a second line rather than the only one. A person editing a layout
  passes through broken states on the way to a working one, and dropping the UI at each of
  them is worse than drawing the last good version.

  **The input scan stays quiet.** It reads every layout before anything cooks, and the rule
  reads the same file a moment later. Both reporting doubled every message a broken layout
  produced.
- **M10.4 reloads a UI image, and four things had to let go of the texture.** The order is
  `ImageFactory` frees it, `UiPass` forgets its descriptor sets, the node tree asks the
  factory again, and the probe asks again. A set naming a freed texture is undefined rather
  than an error, and `moth_ui::NodeImage` keeps the image it was handed with the handle
  inside it. **#439 took the probe away, so three hold it now**, and the lesson is kept here
  because the count is the part that was wrong rather than the ordering.

  **The fourth holder was found by comparing pictures, not by a crash.** A run that swapped
  the image part way through drew a different frame from a run that started with it, and the
  difference was exactly the probe's rectangle. It was binding a freed texture and getting
  away with it. The test that proves this is the three-way one: reload the image mid-run and
  the capture must be byte identical to a run that had the new image from the start.

  **A raw pointer into the runtime does not survive a reload, and that is now twice.** M10.3
  met it with the layout node and M10.4 met it again with the probe image, because
  `FrameContext` held both by raw pointer taken once at start. Both point at the owner now.
  Anything in that struct naming something a reload can replace has to.

  **The separate texture cache is paid on purpose.** `ImageFactory` holds its own rather than
  sharing `render::MeshPass`'s, so an image named by both a material and a layout uploads
  twice. Before M10.4 nothing dropped and the separation bought nothing measurable. Now both
  sides drop and each knows only its own holders, so one shared cache would need one reload
  path that knows every holder on both sides. That is larger than a second upload of an image
  no scene shares. See `src/ui/image.h`.
- **Input bridge.** Translate SDL3 events into moth_ui events. Controller navigation will
  live at this seam.

  **M10.5 built it, and the two questions it had to settle are answered below.**
  `engine::ui::InputBridge` reads a `platform::InputFrame` rather than SDL. M8.0 put input in
  `platform/` so the game and the game UI read one input layer, and going back to SDL here
  would have given the UI a second one. The frame carries no device type, so `src/ui/` names
  none either, and `tests/test_ui_input.cpp` drives the whole bridge with frames written by
  hand and no window.

  **The UI sees a frame before the game, and it can take input away.** An open layout gets
  each event first. When it consumes one, `InputBridge::take` clears that key or button out of
  the frame, so no reader below the UI sees it: not the camera, and not the game. That is what
  lets a pause menu swallow the key that would otherwise move the player. Game first was
  rejected, because the game would then need to know which menus are open, which puts UI state
  into game code.

  **A press the UI takes owns its release.** The claim lasts until the key or the button comes
  up, rather than ending with the frame that consumed the edge. Without it the game reads the
  button as held from the second frame of a drag that began on a menu. The release itself still
  reaches the UI, because that release is what activates a `moth_ui::UIButton`.

  **A move is never taken.** moth_ui declines one, because a hover changes what a widget looks
  like and belongs to nobody.

  **A UI event runs on the frame clock, and the fold into the step happens after it.** The UI
  reads every device frame, so a click answers at the frame rate. The runtime already folds
  each device frame into what the next fixed step reads, and that fold now runs after the UI
  has had the frame, so an event the UI took never reaches the game.

  The fixed step alone was rejected. A frame often takes no step at all, and offscreen it
  almost never does, so a press and a release between two steps would be an edge the UI never
  sees. M8.6 found that problem once already, on the game side.

  **The bridge keeps the frame the devices reported, never the frame it left behind.** A key
  the UI owns reads as up in the frame the caller keeps. Comparing the next frame against that
  would report a press on every frame the person holds the key. Two mutations of this fail four
  and five checks.

  **A reload drops what the UI owns.** The new node tree knows nothing about a key the old one
  took, and holding the claim would leave the game unable to read it until a person let go and
  pressed it again. `reload_ui_layout` calls `InputBridge::forget` for that.

  **`LayoutListener` points at the owner of the root, not at the root.** It is the third thing
  in this section to say so. A capture holder answers before the depth-first broadcast, which is
  what `moth_ui::flow::TransitioningLayer` does: a runtime holding a bare root has to repeat it
  or a widget in an exclusive input mode loses to its siblings. Deleting that path fails two
  checks, and only because the test adds the capture holder as the first child. Added last it
  wins the broadcast anyway and proves nothing.

  **The screen rectangle is set before the events, as well as before the draw.** A layout lays
  its children out from that rectangle and a hit test asks which child a point is in. A layout
  that had never drawn would size every child at zero and answer no click at all.

  **What is not answered is a key that was already down when the menu opened.** An event is an
  edge, so the UI never sees that key and never claims it. Opening a menu is the game's own
  decision, and the game stops reading movement when it makes one. A modal gate that takes the
  whole keyboard belongs with the menu that needs it, which is M10.7.

  **`--click-at-frame` and `--click-at` replay a click, which closed #396.** Every test of the
  bridge drives it with frames written by hand, so `platform::sample` and the order of the three
  calls in `update_input` were verified by reading alone. A wrong order still compiles, passes
  every test, and lets the player walk while the menu is open.

  It is the shape the key replay uses. The flag writes a pointer position and a button into
  `platform::InputFrame` rather than calling into moth_ui, so the click travels the whole path a
  hand drives and a wiring mistake fails the capture rather than passing it.

  **The button comes up on the frame after, at the same point.** A press is the whole gesture and
  a release somewhere else cancels it, so the position is written on both frames. Writing it on
  the first alone puts the release at 0,0 and the capture goes back to the frame with no click in
  it, which is the mutation that proves both frames matter.

  Three runs of one click give one image. A click on the sandbox throw button knocks the stack
  over, and a click that misses it gives the capture with no click in it byte for byte.
- **Lua bindings.** Bind moth_ui nodes through the reflection system. This gives you menus
  driven by script, which is the right way to author UI behavior.

  **M10.6 built the binding half, and the surface is below.** It is chosen one call at a
  time, the way the M8.3 surface was, and rule 4.6 keeps it to what a menu needs.

  ```
  ui.show(layout)              -- loads it if it is not loaded, and shows it
  ui.hide(layout)              -- hides it, and it stays loaded
  ui.visible(layout)           -- whether it is loaded and drawing
  ui.find(layout, node)        -- a handle, or nil when there is no such node

  game.pause()                 -- holds the fixed step. M10.7a
  game.resume()                -- lets it run again
  game.paused()                -- whether the steps are held
  game.quit()                  -- asks whoever drives the game to stop it. M10.7f

  node.layout, node.node       -- what the handle stands for
  node:text()                  -- what a text node shows
  node:set_text(text)
  node:visible()
  node:set_visible(shown)
  node:set_image(image)        -- an image by its source path

  function on_ui_press(layout, node) end   -- the sixth callback
  function on_ui_reload(layout) end        -- the seventh. M10.7c
  function on_paused_update() end          -- the eighth. M10.7d
  ```

  **A layout is named by its source path and a node by its id.** Nothing hands a script a
  pointer to a node. `ui.find` gives back a handle holding those two strings, and every
  method on it looks the node up again through the surface of the current step. So a hot
  reload that frees the whole node tree leaves every handle a script is holding still
  correct. A handle that cached a node would be holding freed memory on the next frame,
  which this section has already recorded three times under a different name.

  The handle is sugar over four flat calls that would each repeat the layout and the node.
  It is worth having for that reason alone, and it is worth having only in this shape.

  **`script::UiSurface` is the seam, and it exists because `engine_core` may not name a
  moth_ui type.** §8.5 keeps game UI optional, so `src/script/` calls an abstract interface
  and `engine::ui::ScriptSurface` is the one implementation. A build with `with_ui=False`
  passes no surface, and then every call answers false rather than failing. That is the same
  rule an unbound input action follows.

  **A press is recorded on the frame clock and delivered on the fixed step.** M10.5 settled
  that a UI event is a frame event, and #245 settled that game logic runs on the step. So the
  presses gather between two steps and `Host::deliver_ui_events` drains them, exactly the way
  `deliver_physics_events` drains the touches of one step. Draining on the frame clock would
  put game logic back on the wall clock.

  **One step delivers every press the frames gathered**, for the reason #263 gives for
  physics events: a frame that ran no step is common, so reporting only the last press would
  lose the rest and nobody would reproduce it.

  **A press goes to every instance that declares `on_ui_press`, in entity order.** A press
  names a node and not an entity, so there is no one entity it belongs to. The instances live
  in a hash map whose walk order is neither creation order nor stable, so the listeners are
  sorted by entity before any of them is called. A reproducible run rests on there being no
  order that nothing promises. See §9.

  **`engine::ui::ScriptSurface` is the implementation, and it owns every layout.** The runtime
  used to hold one layout, loaded at start, and no script could reach it. A script names a
  layout by its source path and the surface loads it on demand, because M10.7 wants a main
  menu, a pause menu and a HUD as three layouts rather than one with three subtrees.

  **Showing a layout raises it.** The most recently shown one draws last and answers a click
  first, so a pause menu over a HUD takes the input and the HUD never sees it. One rule covers
  both orders, and a script that shows the same layouts in the same order every step gets the
  same order back.

  **The surface is the `moth_ui::IEventListener` the bridge sends a frame to.** So nothing in
  the runtime holds a layout root any more, and the raw pointer that M10.3 and M10.4 each lost
  a day to has nowhere left to hide. It builds a `LayoutListener` for each layout as it walks
  them, because that class is what repeats moth_ui's own routing: a captured node answers
  before the depth-first broadcast.

  **A layout is built through `moth_ui::NodeFactory`, never through `Layout::Instantiate`.**
  Instantiate builds a plain group and never asks what class the layout named, so a layout
  whose root is a widget came back drawing correctly and answering nothing. A probe measured
  both: through Instantiate a root-class button reads back as not clickable, and through the
  factory it reads back as clickable. Nothing reported it, which is the shape this section
  keeps meeting.

  **`moth_ui::EnsureWidgetsRegistered` has to be called, and its absence looks like an
  authoring mistake.** moth_ui registers a widget from a static initializer in the translation
  unit that defines it. It is a static library and no engine code names `UIButton`, so the
  linker drops that object file and the registration never runs. A layout naming the class then
  builds a plain group. The surface calls it once when it is built.

  **A layout hot reload throws away what a script wrote.** The nodes are built again from the
  layout file, so a changed text goes back to the authored one. Whether a layout is showing,
  and where it sits in the order, both survive. This is the same rule M8.5 settled for the
  script table: a node is scratch and a component is storage.

  **M10.7c tells the script, which is what makes that rule liveable.** `on_ui_reload(layout)` is
  the seventh callback, and it goes to every instance that declares it, in entity order, under
  the same rule a press follows. `ScriptSurface` reports each layout it rebuilt and
  `Host::deliver_ui_events` drains the reports beside the presses.

  **The engine cannot put those values back itself**, which is why this is a signal rather than a
  fix. It never knew which text came from a script and which from the file, and a surface that
  kept every write would be a second copy of the layout that no reload could ever correct.

  **A reload is delivered before a press of the same batch.** A press acting first would act on a
  menu still reading whatever its file says. One test drives both in one drain and checks the
  order, because the other order is what falls out of writing the loops the obvious way round.

  **An image reload reports too.** `Node::ReloadEntity` builds every child again, so it loses a
  script's text exactly as a layout reload does. That was silent, and the loss looked like a
  fault in the game rather than in the reload.

  **It arrives on the frame clock while the game is paused**, because `Session::advance` drains
  the UI events on its paused branch. A menu is edited while the game is paused, so a reload that
  only ever reached a step would leave every label wrong for exactly as long as somebody was
  looking at it.

  This is what #410 asked for, and it is the gap the M10.7 authoring pass rated first. Before it,
  the first save while authoring a menu made every button in that menu read `Button`, because a
  referenced button cannot carry a per-instance label in the file. See #402 and §10 M10.

  **M10.7d gave a paused game one callback of its own, which closed #408.** A script reads an
  action inside `on_update`, and a paused session runs none, so the key that would resume a game
  could never be seen. `on_paused_update()` runs once for each frame while the game is paused,
  and the actions move on that clock so an edge reaches it.

  **A separate callback rather than running `on_update` with a zero delta.** The cheap answer
  makes "a paused game runs no `on_update`" untrue, and that rule is what keeps a pause from
  moving anything. Every game would then have to test `game.paused()` at the top of `on_update`
  or quietly keep running. A callback that exists only while paused cannot be got wrong that way.

  **It takes no seconds.** A paused game runs no simulated time, and the wall clock is what #245
  took out of a script. What it is for is the key that resumes, and nothing that moves the world.

  **It syncs the instances the way a step does**, so a script edited while the game is paused
  restarts there and then. Editing a menu while it is on the screen is exactly when that matters,
  and the sync is shared rather than copied so the two paths cannot disagree about which
  instances exist.

  **A pause still takes the key away from the game.** The edges are consumed on the paused frames,
  so the first step after a resume does not take every key pressed while the menu was up. That
  guarantee came with M10.7a and it is now measured through the general flag: a throw replayed
  during a pause gives the paused capture byte for byte.

  **M10.7e draws what a paused game moved, which closed #409.** Both interpolate calls sat at the
  end of the step loop, so a paused advance wrote no drawn pose at all and anything moved while
  paused kept its old one until something resumed. A paused advance runs them now, with the alpha
  the clock already had. Nothing stepped, so every blend gives back what the last frame drew, and
  only a pose something wrote while paused moves.

  **`physics::Simulation::teleport` was already right.** It writes both halves of the pair it
  blends, and has since M8, because setting the newer half alone draws a body sliding across
  everything between where it was and where it was put. So the body half needed no change: it
  needed the interpolation to run at all.

  **`scene::StepMotion::record` was not.** It shifts the pair, which is right inside a step and
  wrong outside one: a transform a script writes while paused would be blended towards and left
  part way there for the whole pause. It writes both halves outside a step now.
  **`begin_step` and `interpolate` are what mark the two**, so nothing has to tell that class
  whether the game is paused, and no caller can forget to.

  The sandbox meets this on the Main menu button, which puts the room back while the game is
  held. The capture is a weak witness for it, because the title screen covers the crates the reset
  moves and only their shadows show, so the check is `tests/test_session.cpp` reading the pose
  back rather than a picture.

  **`runtime --key <frame>:<action>` replays a press by action name**, which replaced
  `--throw-at-frame`. The old flag named `sandbox::kThrowKey` from inside the runtime, so the
  application knew a key that belonged to the game. `platform::Input::keys_of` answers what an
  action was bound to, so the flag now names none and a game that rebinds one needs no change.
  One hook covers every action a game has, which is what a pause menu needed and what the throw
  hook could never have given.

  **`Node::ReloadEntity` destroys and builds every child node again**, so every click action
  wired into the old nodes goes with them. `reload_images` wires them again. Without that a
  button answers once and is silent after the first image reload, and nothing reports it.

  **A button is a referenced layout, and the id belongs to the reference.** moth_ui reads a
  widget class only for a group entity, and the only group a `.mothui` can name as a child is a
  reference. So `button.mothui` carries `"class": "button"` on its root, a menu holds a
  reference to it under whatever id that menu chooses, and `LayoutEntityRef::CopyLayout` brings
  the class across. Two menus stand up the same button file under different names.

  **A layout is not a tree of in-file groups, and adding one was the wrong fix.** A first
  attempt at M10.6 put a `Group` entity into the format, because a probe showed a
  `{"type":"Group"}` child being dropped. The facts were measured and the design intent was
  not: the format nests through references on purpose. A probe against unmodified moth_ui 1.8.0
  then showed a two-button menu of references already working. See moth_ui #154, closed.

  **What was actually missing is that a reference read a file.**
  `LayoutEntityRef::Deserialize` joined the stored path to the directory the referencing layout
  came from and called `Layout::Load`. This engine hands moth_ui bytes out of
  `assets::Content`, so there is no directory and every reference failed. moth_ui 1.9.0 adds
  `ILayoutProvider`: with one set, a reference asks the consumer for its target by `AssetId`,
  and without one the filesystem route runs unchanged.

  `engine::ui::read_layout` supplies that provider and is therefore re-entrant, because moth_ui
  asks for a sub-layout part way through reading the layout that names it. **It keeps the chain
  being read**, so a layout that refers to itself is reported rather than followed until the
  stack runs out. Deleting that guard segfaults `tests/test_ui_layout.cpp` rather than failing
  a check, which is what makes it worth having.

  **The cooker rewrites `layoutPath` the way it rewrites `imagePath`**, and a referenced layout
  is an input of the layout that names it, so editing a button re-cooks every menu that stands
  one up. `assets::kCookerVersion` is 10 for that: the freshness check compares identities,
  input names and input bytes, and a rule that starts writing a new form moves none of them.

  **A node id is unique only within the layout that declares it.** Two references to one button
  file each hold a child called the same thing, and `FindChild` answers with the first. M10.6
  worked around it by putting the labels in the menu beside the references, which works and does
  not scale: a widget with any inner state a script must drive cannot be referenced twice.

  **M10.7 settled it with a path, and moth_ui did not change.** A node name may carry
  `script::kNodePathSeparator`, and `ScriptSurface::node_of` resolves one segment at a time
  inside what the segment before it found. So `"play button/label"` is the label of that one
  reference. `FindChild` already searches a whole subtree, so it gives the per-segment step and
  a name with no separator in it still reaches a node at any depth. That is what keeps every
  M10.6 call working unchanged.

  **A press reports the same path.** It has to, because a script hands a press straight back to
  `ui.find`, and two names for one node would be two vocabularies. A button a menu declares
  itself reports one segment, exactly as before, and a button inside a referenced layout carries
  the reference in front of it.

  The two rejected answers were `moth_ui::LayoutEntityRef::propertyOverrides`, which writes
  `visible` and `blend` and would have to learn a text, and making an id unique at copy time,
  which changes what `moth_editor` shows a person. A lookup rule costs neither.

  **The sandbox button carries its own label now**, and `scripts/puzzle.lua` writes each one
  through the reference that stands it up. The picture does not move by one byte, because the
  labels land in the rectangles the menu used to place them at. That is the check: writing them
  through a bare name puts both texts into the first reference, and the capture moves.

  **M10.7a lets a script pause the game, and `script::GameClock` is the seam.** A pause menu has
  to stop the fixed step, and `play::Session` owns it. `src/script/` sits below `src/play/` and
  may not name it, so the interface goes in `src/script/` and the session implements it. That is
  exactly the shape `script::UiSurface` already has, and a build that passes no clock leaves the
  `game` table answering false.

  **A paused session runs no step at all**: no `on_update`, no solver, and no physics events. It
  advances no clock either, so a pause of any length costs the step after it nothing. Letting the
  clock accumulate instead would pay every second of the pause back at once, hit the ceiling in
  `FixedTimestep`, and report a run that fell behind.

  **A paused session still delivers UI presses, and it has to.** `Host::deliver_ui_events` runs
  inside the step loop, so a pause that ran no step would deliver nothing, and the menu it put on
  the screen could never resume the game. So while paused the presses are delivered once for each
  `advance` rather than once for each step. That is the clock they were gathered on in the first
  place, which is the M10.5 rule rather than an exception to it.

  **A pause restarts the input fold.** `pending_` is an OR across every device frame since the
  last step, and a pause runs none. Without a reset the first step after a resume would hand the
  game every key anybody pressed while the menu was up, all down at once. That mutation passed
  every test the first time it was tried, which is why `tests/test_session.cpp` now presses a key
  during a pause and checks that the game never sees it.

### 8.5 Boundaries

- Keep it an optional dependency behind the `with_ui` Conan option. `engine_core` must not
  depend on it. This costs nothing and keeps a replacement possible.
- Leave `moth_editor` as a standalone tool for now. It works. You can fold it into the
  engine editor later, because both use ImGui. It is not a milestone.
- Keep moth_ui in its own repository with its own release cadence. Consume it by version
  pin. Do not vendor it.
- **The engine consumes the moth_ui 1.x line, and the pin ceiling is load-bearing.**
  `moth_ui/2.0.0` is a separate fork for a larger toolkit that also carries `moth_core`. It
  is not the next version of the line this engine uses. So `[>=1.9 <2]` means what it says,
  and widening the ceiling takes the engine to a different library rather than a newer one.

  **The git tags run ahead of the published line, and an unpublished tag is still a claim on
  a number.** Artifactory holds 1.0.0, 1.1.0, 1.1.1, 1.1.2 and 1.1.3. The repository also
  carries tags 1.5.0, 1.6.0 and 1.7.0, made between 2026-03-25 and 2026-04-04 and never
  published. So the published line never went backwards. The tags sit above it.

  That mattered, because `conan create` on the 1.7.0 tag produces a real 1.7.0, and a
  version range takes the highest it can find rather than the newest. A release numbered
  1.2.0 would lose to it.

  **The answer was to move the version line above the tags rather than to delete them.**
  M10.1 released 1.8.0 and M10.6 released 1.9.0, so the floor this engine pins sits above
  every tag that was ever made. A stale tag can no longer satisfy the range, whoever builds
  it. That is structural, where deleting a tag depends on nobody having fetched it. The
  three tags stay where they are, and they are a record of what the repository did rather
  than a hazard. Issue #390 closed on this.

  **Raise the floor with the release, not after it.** The pin says which moth_ui the engine
  needs, so it is part of the change that needs one. It is also what keeps the floor above
  the tags without anybody thinking about them again.

- **spdlog decides fmt for the whole moth stack, and the declaration order decides
  whether the graph resolves.** spdlog pins one fmt version exactly: 1.14.1 pins 10.2.1
  and 1.17.0 pins 12.1.0. moth_ui asks for `fmt/[>=10.2 <13]`, a range that spans both,
  so moth_ui follows whichever spdlog is in the graph rather than choosing for itself.

  This engine takes spdlog 1.17. moth_graphics and moth_packer took 1.14, so the stack
  shipped moth_ui in two binary flavours and any graph holding both halves refused to
  resolve. moth_editor is the first graph that holds both, and it worked around it with
  an `fmt/10.2.1` override in each consumer. That is a fix every future consumer has to
  repeat and can only find by hitting the conflict. Issue #392.

  **A Conan version range takes the highest version it can find, so the first of the two
  it meets wins.** Meet moth_ui first and its range resolves to the newest fmt, which
  then conflicts with the exact version spdlog pins. Meet spdlog first and its pin is
  already in the graph, so the range resolves onto it. This engine names `spdlog` as a
  direct requirement and has therefore never hit this. moth_editor listed moth_ui first
  and always did.

  So there are two rules, and both are needed. Every moth package takes the same spdlog
  this engine takes, and every consumer of more than one of them names spdlog before it
  names moth_ui.

- **A Conan editable is for development, not for a build somebody else runs.** Develop against
  the editable when a change spans both repositories. Then release moth_ui and move the engine
  to the pin. A green build that only works because of a local editable is a broken build for
  everybody else, and CI is where that surfaces.

**One caution.** An engine plus a UI library is two projects, and the engine alone takes
years. This is acceptable only because moth_ui already exists and works. The extra cost is
integration plus a few widgets. If you start a UI-library refactor while the renderer is
half finished, you have inverted the priority. Rule 4.6 applies with full force here.

### 8.6 What the M6 spike found

M6 exists to answer one question: **is `gfx::` ready for a consumer that is not this
engine?** moth_ui is the only external consumer the engine has, so it is the only thing that
can test that. This section is the answer. Section 8.2 explains why the answer matters more
than the pixels.

**We own both sides, so each finding names the side its fix landed on.** That is the part
worth reading. It is easy to hide a bad interface by patching whichever repository is
convenient.

#### `gfx::` came through well

**Nothing forced a Vulkan detail through, and rule 4.1 held.** `src/ui/` names no Vulkan type
and includes no Vulkan header. The `vulkan-containment` CI job covers it. The whole game UI
sits on `create_texture`, `create_graphics_pipeline`, `create_descriptor_set`, `update_buffer`
and the command recording calls, and none of them leaked a `Vk` type.

**`gfx::` would survive a C ABI today, with one exception already known.** The interface the
UI uses is opaque handles and POD structs, which is what rule 4.2 asks for. The exception is
`gfx::Result` and the `succeeded()` helper, which are fine, and the descriptor and pipeline
descriptors, which are aggregates of PODs and arrays. Nothing in the UI path takes a
`std::string` or a `std::vector` across the boundary.

**One `gfx::` gap is real and it is the sampler.** A sampler belongs to the texture it was
created with, so a consumer cannot choose a filter when it binds. moth_ui asks for one per
image, and the recorder carries the request and breaks the batch on it, and then `UiPass`
cannot honour it. Issue #209 holds it. The better shape is a sampler as its own handle, bound
beside the texture, which is what Vulkan does underneath anyway.

**A second gap is smaller.** The frame report times the shadow, cull, mesh and tonemap passes
and not the UI pass, so the cost of the game UI is invisible next to everything else. That is
an engine gap rather than a `gfx::` one.

#### moth_ui interfaces that are wrong for a runtime

These are the findings the milestone was really for. Each one is an interface that assumes a
game reads loose files from a disk, which a cooked runtime does not.

**`IImageFactory::GetImage` takes a filesystem path, and moth_ui makes it absolute.**
`LayoutEntityImage::Deserialize` stores
`std::filesystem::absolute(layout directory / stored path)`. The engine names an asset by a
source path and the manifest is keyed on one, so an absolute path matches nothing. The engine
undoes it in `engine::ui::source_path_for`, which is correct only because the cooked tree
mirrors the source tree.

*The fix landed on the engine side, and that is the wrong side.* The right shape is an asset
identity that a layout carries and a backend resolves, rather than a path moth_ui rewrites on
the way through. That is a moth_ui change and it is not a small one, so it is filed rather
than done inside a timeboxed spike.

**`IFontFactory` makes a runtime implement editor project files.** `LoadProject`,
`SaveProject` and `GetCurrentProjectPath` exist for `moth_editor`, which keeps its font list
in a JSON file. A runtime takes its fonts from the cooked tree, so implementing them would
create a second source of truth. `engine::ui::FontFactory` asserts and logs in all three. The
seam belongs one level down: an editor-only interface that extends the runtime one.

**A font is named and an image is pathed, for no reason a consumer can see.** A layout stores
a font by a registered name and an image by a path. So the game must register every font name
before a layout loads, and must register no image. One rule for both would remove a class of
mistake.

**`moth_ui::IFont` declares no methods at all.** Section 8.3 already said this. The measured
cost is below.

**One enum in the layout format is stored differently from its neighbours.** `imageScaleType`
is a string and `textureFilter` is a number, in the same file, written by the same
serializer. Every layout in every moth project agrees: 25 files store `"textureFilter": 0`
and none stores a name. Hand-authoring a layout with the name throws
`type must be number, but is string`, which names no field. This cost real time in M6.5 and
it is filed against moth_ui.

#### What text actually cost

Section 8.3 warned that people underestimate this. The warning was right, and the reference
made it cheaper than it would have been:

| Part | Lines |
|---|---|
| `moth_graphics` reference, for comparison | 441 |
| `src/ui/font.h` and `font.cpp` | 981 |
| `src/ui/font_factory.h` and `font_factory.cpp` | 322 |
| `tests/test_ui_font.cpp` | 428 |

So text is about 1300 lines of engine code and 400 of test, against a 441-line reference. The
difference is documentation, the split of `load()` from `upload()` that makes the whole thing
testable with no GPU, and the two parts that were rewritten rather than ported. It is roughly
half of everything `src/ui/` holds, for one of the five things a UI draws.

Text was also where every real bug lived. The alpha blend that `RenderText` forces, the blank
line that `wrap()` used to drop, and the advance that used to truncate were all found after
the code worked.

#### What was fixed here, and what was filed

Fixed in this milestone, engine side: the sampler request is carried rather than dropped, the
layout path is resolved against the manifest, text forces its own blend mode, and the three
editor-only factory methods assert and log.

**M10.1 moved the second of those to the side it belonged on.** The spike said the path fix
had landed on the wrong side, and it had. `moth_ui::AssetId` is now an opaque identity that
moth_ui carries and never reads, `LayoutEntityImage` stops rewriting what it stores, and
`engine::ui::source_path_for` is gone. What replaced it, `manifest_key_for`, validates an
identity rather than undoing something moth_ui did to it. That closed #218.

Filed: #209 the sampler, #210 UI hot reload, #213 on-demand glyph packing, #214 one atlas for
each size, #216 shaping direction, and the moth_ui interface findings above. **None of the
moth_ui findings were fixed here, so no moth_ui release was needed and the engine still pins
`moth_ui/[>=1.1.3 <2]`.** The local editable is `moth_ui/1.1.1`, which that range excludes, so
it is not load-bearing and the build takes 1.1.3 from the cache.

---

## 9. Cross-cutting notes

**Time model.** Use a fixed timestep for physics and game logic, and interpolate for
rendering. Decide this now. Scripting, determinism, record and replay, and any later
networking all depend on it. Box3D supports deterministic replay if the simulation stays
deterministic.

**Which keys an application keeps, and which a game may bind.** M10.7f settled it, because a
pause menu wanted Escape and the runtime was quitting on it.

- **A game binds whatever its own actions need**, through `sandbox::bind_actions` or its
  equivalent. It names the key and the engine names none of it: `runtime --key` replays a press
  by action name and asks the input what that action was bound to.
- **An application keeps only the keys that drive the application.** The editor keeps Escape to
  clear the selection and turns `platform::WindowDesc::quit_on_escape` off for it. The runtime
  keeps the camera keys and now turns that flag off too.
- **The way out of the runtime is the game's, not the application's.** `game.quit()` asks and
  `script::GameExit` is the seam. That is what freed Escape: a key the application used for the
  one thing a person cannot otherwise do is not a key a game can have.

**A quit is a request rather than an exit**, which is what lets the two applications mean
different things by it. The runtime leaves its frame loop. The editor stops play mode and gives
the world back, because a game that could end that process would take a person's unsaved work
with it. Nothing in `src/script/` may call `exit()`.

**Memory.** Use a per-frame linear arena that resets each tick, plus pool allocators for hot
component types. Do not write a full custom allocator stack. These two save more time than
they cost. The rest do not.

**Scene graph.** EnTT has no parenting. The transform hierarchy is engine code: parent and
child links, local and world transforms, dirty propagation, correct update order, and
reparenting with no visual jump. It is more complex than it looks.

**Physics and the transform: `BodyType` decides which way the data moves.** A rigid body
integrates a position and the M3.1 hierarchy holds one, so something has to say which is
authoritative. Both, at once, gives a body that fights its parent or snaps back every frame.

The rule is one direction for each body, chosen by what moves it:

- **Dynamic.** The solver owns it. After the step, the body transform is written back to the
  entity. Nothing pushes a transform into a dynamic body during a step.
- **Static and kinematic.** The entity owns it. Before the step, the entity world transform
  is pushed into the body. The solver never moves one, so nothing is written back.

That falls out of what the three types mean. A dynamic body ends up somewhere only the
solver knows. A static body never moves. A kinematic body is moved by game code or by an
animation, and the solver reads it so that dynamic bodies get pushed aside correctly.

**A dynamic body sits at the root of the hierarchy, and the engine refuses one that does
not.** Box3D works in world space and `Transform` is local to the parent. A dynamic body at
the root makes those the same thing, so the write-back stores what the solver produced and
converts nothing.

The refusal is the point rather than a shortcut. Parenting means "this goes where that goes",
and a dynamic body goes where the solver puts it. The two cannot both hold. Allowing the
parent link would leave it in place meaning nothing: moving the parent would change the
numbers stored in the child and not where it appears, because the next step overwrites them.

Converting world to local instead was considered and rejected. It costs one matrix inverse,
which is nothing, and one invariant that nothing enforces, which is not. The write-back would
have to walk parents before children and rebuild each world matrix as it went, because a
child needs the parent position from this step and not from the last one. Get that order
wrong in a stack and each level is a little further out than the one below. The picture looks
like an unstable solver rather than like a bug in a loop, which is the failure `DESIGN.md`
§3 exists to warn about.

**This restricts dynamic bodies alone.** A static or kinematic body under a parent is correct
and useful, because the entity owns its transform in both cases. A platform that is part of a
lift assembly is exactly that.

A jointed assembly does not need the hierarchy either. A ragdoll or a hinge is physics joints
between bodies that are each at the root, which is what Box3D provides and what stays correct
under the solver.

Going the other way later is cheap, and this direction is not. A build that allows a parented
dynamic body can start converting whenever a real case turns up. A build that already has
content relying on it cannot take that back.

The write-back deliberately leaves scale alone, because a rigid body has none to give. The
collider reads it instead, which #237 closed.

**A collider is sized in meters and then multiplied by the world scale of its entity.** So one
crate prefab makes a big crate and a small crate, which is what prefab instancing exists for.
A box takes a scale of three different numbers exactly, because a box is axis aligned in the
frame of its entity. A sphere holds one radius, so it takes the largest of the three and names
the entity in a warning. The largest rather than the smallest, because a collider bigger than
the picture holds the body up and a smaller one lets it sink through the floor.

Box3D fixes the size of a shape when it creates it, so a scale that changes after the body
exists needs a new shape. `Simulation::step` compares the world scale of every body against
the scale its shapes were built at, and rebuilds when they differ. The body survives the
rebuild, so a crate resized while it falls keeps its velocity and its contacts rather than
stopping dead in the air.

The compare carries a tolerance of 1e-4. A world matrix decompose leaves rounding that moves
with the rotation, so an exact compare rebuilds a resting body every step, throws its contacts
away each time, and the body never settles. `Simulation::shape_rebuild_count` is what measures
that rather than asserting it: a scene that resizes nothing reports zero.

**Time model.** Use a fixed timestep for physics and game logic, and interpolate for
rendering. See the note at the top of this section.

M7.4 built it. `engine::FixedTimestep` in `src/core/timestep.h` turns a frame delta into whole
steps and reports how far the frame sits into the step that has not run. `Simulation::step`
records the pose of each dynamic body at the last two steps, and `Simulation::interpolate`
writes the blend of them. So the step no longer moves an entity, because a frame runs zero,
one, or several steps and then draws once.

**The accumulator has a ceiling, and a frame that hits it throws the rest away.** A frame
longer than one step leaves time owed, and paying it back makes the next frame run several
steps, which takes longer still. That is the spiral of death. The default is five steps, and
`dropped_seconds()` reports the simulated time the run will never make up, so a machine that
cannot keep up says so rather than quietly running slow.

**The game logic is on the fixed step too**, which #245 closed. The game takes simulated
seconds, which is the step count times the step length, rather than the wall clock. So the game
is a function of how many steps have run and of nothing else. A replay that feeds the same
input lands on the same values. M8.6 moved that logic into Lua and the rule went with it:
`script::Host::update` takes the same simulated seconds.

**The scene owns the camera, and `editor::ViewSettings` no longer does.** M9.5a made
`scene::Camera` a reflected component. The pose is the entity's transform, so a camera is
moved and turned the way a light is, and the component carries the field of view, the near
plane, and the exposure. `scene::primary_camera` picks the one a game plays through.

Two things follow that are worth writing down. **The choice is by entity, not by the order
EnTT hands the cameras over**: a pool order is neither creation order nor stable, so a scene
with two cameras would otherwise answer differently after any component was added anywhere.
And **the view matrix is the inverse of the world matrix** rather than a `lookAt` built from
two angles, which is what keeps a camera under a moving parent correct and what lets a
camera carry roll.

The picture moved by 133 pixels of 921,600 when this landed, by at most 2 of 255, all of it
on geometry edges. That is the arithmetic changing and not the camera: a matrix inverse
rounds differently from the `lookAt` the runtime built before, and the authored quaternion is
a different route to the same pose. A wrong pose moves the whole frame rather than its edges.

**`play::Session` is that whole step in one place**, which M9.4a moved out of
`apps/runtime/main.cpp`. It owns the clock, the solver, the script host, the step input, and
the poses a frame blends, and `advance()` runs the steps one frame owes. The order inside it
carries rules that were each paid for once: the game runs before the solver so a kinematic
body carries its new transform into the step, the physics events are read inside the loop
because the simulation keeps one step of them, and one alpha blends both so the game and the
physics draw the same instant.

The editor plays a scene as well as the runtime, and a second copy of that loop would let the
two applications run the same game differently. That is the one thing play-in-editor cannot
do. It is its own directory rather than part of `scene/` or `physics/`, because it sits above
both and above `script/`, and none of those three knows about the others.

**`scene::StepMotion` is where the interpolation for a mover that is not a rigid body lives.**
This is the design question #245 posed. `physics::Simulation` already blends the last two poses
of a dynamic body. Game logic has the same problem and no solver to ask. So `StepMotion` is that
mechanism with the game as the author. It sits in `scene/` because it records transforms and
names neither a physics type nor a game type. It holds no reflected component either. These
poses are the state of a run rather than something a person authors, so none of them reach a
scene file.

`StepMotion` also drops an entity the world no longer holds. A reloaded scene clears the
registry, and EnTT then hands the same numbers out again with a new version. A stale handle can
therefore name an entity somebody else now owns, and `World::set_local` does not check. The
runtime clears the poses at the reload as well, beside the selection it already clears.

**A step reads the last step, not the blend the last frame drew.** This is the part that is
easy to miss. The transform on an entity is whatever the frame drew, and that is a blend of two
steps rather than the result of one. A step that read it back would feed the motion of a frame
that fell between two steps into the simulation, and it would compound. `StepMotion::begin_step`
puts the authoritative pose back first, and the pose it holds is the one that counts.

One alpha drives both blends. Two would let the game and the physics draw different instants of
the same frame.

**A dynamic body never reaches `StepMotion`, and a script that writes its Transform is told
so.** The two mechanisms answer the same question, and only one of them can own a pose. The
solver owns a dynamic body, and `StepMotion` writes last, so recording one there freezes it: the
body integrates, the velocity reads back correctly, and the position stands still. Nothing else
looks wrong, which is what made issue #284 expensive to find.

So `script::Host` checks the body before it records. A dynamic body gets a warning naming the
entity and pointing at `teleport`, and the pose is left to the solver. A static or a kinematic
body is the other way round, because there the entity owns the pose and `step()` reads it, so
those keep their interpolation. The warning is said once for each entity, because a script that
writes a Transform usually does it on every step.

**The same wall time is not the same number of steps.** In float, 288 frames of 1/144 sum to
just under two seconds. That run takes 119 steps where a 30 Hz run takes 120. This is the frame delta accumulating and not the step drifting.

So determinism means that the same step count gives the same answer.
`tests/test_step_motion.cpp` holds the step count fixed rather than the wall time. The drawn
pose still differs between two frame rates by up to one step of motion. That difference is the
interpolation doing its job.

Simulated seconds are a `double`, all the way into the game. A `float` resolves steps of 1/60
until about three days of running. Past that, two steps in a row land on the same number and the
game stops advancing.

Working the step count out by dividing the accumulator was wrong. A step of 1/60 is not a
number a float holds, so one whole second divided by it gives 59.99998 and truncates to 59. A
simulation would lose a step of every second it ran. It subtracts in a bounded loop instead.

The drawn pose is up to one step behind the solver. That is what interpolating between two
states that have already happened costs. Extrapolating past the newest one would remove the
lag and overshoot every collision, because the newest state is exactly where the solver has
not yet decided what happens next.

**Debug draw.** M7.5 reads the wireframe out of Box3D rather than out of the components the
engine holds. A wireframe built from our own components agrees with the mesh even when the
solver disagrees with both, and a collider that does not match its mesh is the thing it exists
to find. It draws after the tonemap curve and tests no depth: after the curve so ACES cannot
move the color that says what state a body is in, and without depth so a collider inside
geometry still shows.

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

**A field names an asset because it says so, not because of its text.** Both directions used
to read the text of every string in the document. That is the wrong granularity. A field means
something because of its type, so an entity named after a cooked GUID had its name replaced on
save, and the scene then held a name nobody wrote.

`reflect::AssetRef` marks a field that names an asset. `ComponentOps::reference_field_names`
holds those names for each registered component, filled from the descriptors, and
`scene::for_each_reference_field` is the one walk over a document that reads them. The cooker
resolves through it and a save restores through it, so the two cannot disagree about what a
reference is. The walk sits in `scene/` because it needs the component registry, and `assets/`
cannot depend on `scene/`. The manifest arrives as a parameter instead.

**So the cooker has to hold the descriptors of every component a document can carry.** The
engine's own are not enough, because a game defines its own types and one of them may name an
asset. `engine::import` therefore takes the registry as an input and knows no game, and the
`cooker` executable links the game module and registers it, exactly as `apps/runtime` does.
That makes the cooker specific to one game. It is the same compromise the runtime already
makes, and it is one place rather than two when a project system chooses a game instead.

A string that starts with `asset:` and sits anywhere else fails the cook, naming the document
and the text. The resolve step never reaches it, so the alternative is a cooked document
carrying text where the runtime reads a GUID. That covers a reference typed into an ordinary
string field, and a component nobody registered.

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
wrong for three megabytes one shader fills and another reads. `BufferDesc::memory` is the
answer, and `update_buffer` refuses a device-local buffer rather than writing through a pointer
that is not there.

**That field started as `device_only` and became `BufferMemory`, which closed issue #204.** The
boolean said half of the question and had nothing to say about the other half: a vertex buffer
the host writes every frame. `gfx::` refused one outright. `create_buffer` would not take a
vertex or an index buffer with no data, and `update_buffer` would not write one, so per-frame
geometry had to destroy its buffer and build another from the recording on every frame.

That is what `engine::ui::UiPass` did. It cost an allocation and a free for each of two buffers
on each frame, and it was a correctness trap besides: destroying the buffer the previous frame
is still reading is a real error, and synchronization validation reported it. Every later
consumer, and M7 debug draw and particles are the ones in view, would have rediscovered the
same trap.

`BufferMemory::HostVisible` allocates one now and lets `update_buffer` fill it. `UiPass` grows
a buffer only when a recording outgrows it, by half again so a recording that creeps upward
does not reallocate every frame: 6 allocations over 300 frames, which is one for each of two
buffers in each of three slots, against two on every frame that drew.

**The frames in flight stay the caller's problem, and the doc says so.** `update_buffer` writes
straight into memory the GPU may be reading, and nothing in `gfx::` tracks which frame last read
a buffer. The slot ring in `UiPass` is what answers it there.

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

**`render::SceneRenderer` holds the pass order, and an application holds the frame.** Which
passes a frame runs, what each one declares, and where the barriers between them go is one
answer. It sat inline in the frame loop of `apps/runtime` until M9.3, which was fine while one
program drew a scene. The editor draws the same four passes into a panel, so a second copy of
that order would have to be kept in step by hand.

The split is between the scene and the frame. `SceneRenderer` owns the shadow pass, the mesh
pass, the tonemap pass, the carried resource states, and the GPU timestamps. It draws a
`scene::World` through a `SceneView` that carries the camera and the size, and it names no
window and no swapchain. The application opens the frame, decides what the tonemapped picture
is written into, and draws whatever goes over it.

The tonemap is a call of its own for that reason. The physics wireframe, the game UI, and the
ImGui overlay all draw in the same rendering scope, after the curve, and only the application
knows which of them it has.

**Where the tonemap writes is what separates a runtime frame from an editor frame.** A runtime
maps the scene down straight into the swapchain image and the overlay floats over it. An editor
maps it into an image of its own, and then the overlay samples that image and draws it inside a
panel. So `SceneView::output` is the whole difference, and a null handle means the swapchain.

That adds one producer and consumer pair, and the graph derives it like any other.
`kViewportColor` is the image, the tonemap pass declares it as the write when a caller named
one, and a trailing pass declares the read. A runtime frame never declares that pass, so its
barriers do not move.

The scene renders at the size of the panel rather than the size of the window. Both images
follow it, because the passes draw into the half float one and the tonemap writes the other, and
sizing them apart would render the scene at one aspect and show it at another. The camera aspect
comes from the same size, so a person dragging a panel edge gets more or less of the scene rather
than a stretched picture.

**A target cannot be rebuilt while a frame is recording.** So the panel reports the size it wants
and the rebuild happens at the top of the next frame, where it can wait for the device. One frame
of a dragged edge therefore shows the picture at the size before the drag.

**An editor viewport is clamped to the swapchain image.** `gfx::cmd_begin_color_rendering`
attaches the frame depth image, which is the size of the window, and a render area has to fit
inside every attachment. A docked panel is always inside the window, so the clamp is a guard
rather than a live condition until #306 makes a panel an OS window of its own.

The tonemap scope attaches no depth at all, and that is not an optimization. The triangle
declares no depth format, Vulkan compares the pipeline against the attachments at every draw, and
attaching one anyway is an error. `cmd_begin_color_rendering` takes the same `attach_depth` that
`cmd_begin_rendering` always took.

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

**The cooker reuses geometry between two meshes that name the same accessors**, which saves
building tangents and running both meshopt passes again. The key has to describe the whole
mesh, and #254 is what happens when it does not. It described the first primitive alone while
the value held every primitive, so two meshes that agreed there and differed after it shared a
buffer. The reuse replaces the vertices and the indices and leaves the submeshes alone, so the
submesh ranges of the second mesh then indexed into the geometry of the first. Leaving TANGENT
out of the key had the same shape: two meshes supplying different tangents shared the first
one's, and the normal map lit the wrong way. Neither reported anything, because a cook that
writes the wrong bytes still succeeds.

So the key is every primitive in order, with every accessor that feeds a `MeshVertex` and the
primitive type. Order counts as well as the set: the same primitives in another order lay the
vertices out differently, so the submesh ranges differ even though the geometry does not.

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

### M5 — PBR and render graph — complete
A frame graph that handles barriers and transient resource aliasing. Cook-Torrance
metallic-roughness. IBL: an HDR environment converted to irradiance SH, prefiltered
specular, and a BRDF LUT. A clustered light cull, which the graph schedules. Cascaded
shadow maps. ACES tonemap. Materials as a shader plus a reflected parameter block.
**Done when:** a Sponza-class scene renders correctly.

Intel Sponza renders correctly. It is 3.75M triangles over 115 meshes and 28 materials,
lit by a sun with four cascades, 22 lamp point lights, and image based lighting from the
sky it ships with. `scenes/sponza/` holds it and `scripts/fetch-scene.py` brings the model
down, because 133 MiB of geometry in one file is more than GitHub accepts. See issue #130.

At 1280x720 on the reference GPU the frame is **20.3 ms**, repeatable to 0.5 percent over
three runs of 600 frames. The shadow pass is 8.07 ms of it, the mesh pass 7.30 ms, the
light cull 0.025 ms, and the tonemap 0.162 ms. Synchronization validation reports nothing
over 300 frames on that scene. Issue #194 holds the shadow cost, which the sandbox was far
too small to show.

One piece of the definition above did not land, and it has a reason rather than an excuse.
**Transient resource aliasing** is issue #122, and its trigger condition is still not met:
the shadow map, the scene colour and the cluster grid all overlap at the mesh pass, so no
two of them have disjoint lifetimes and there is nothing to alias. Rule 4.6 says to build
it when something needs it.

**Drawing the environment was the other one, and it landed after the milestone as #193.**
The milestone asked for IBL and got it. Drawing the sky is a separate pass, and Sponza was
simply the first scene to want it.

`render::SkyPass` is one full-screen triangle at the far plane, drawn after the geometry.
The depth test rejects every pixel something else covered, so it shades only what the frame
would otherwise show as its clear. It costs 0.001 ms.

**The proof is which pixels moved, not how many.** On Sponza with the camera pitched up at
the colonnade, the sky changes 54,673 pixels of 921,600. Every one of them held exactly the
clear colour in the capture without the sky, and no other colour appears under the changed
set. Deleting the `Environment` component from that scene leaves exactly 54,673 pixels
holding the clear colour. So the sky covers precisely the clear-colour set, no more and no
less, and no shaded pixel moved.

**It is a pass in the code and not in the graph.** It draws inside the rendering scope
`MeshPass` opened and touches the same two attachments in the same two states
`MeshPass::declare()` names. A declaration of its own would derive a barrier, because
`derive_barriers` orders every write against what came before it rather than only a write
in a new state, and issuing a barrier inside a rendering scope is invalid.

**`gfx::GraphicsPipelineDesc::depth_equal` came from this.** The compare is greater-than,
which is what reverse-Z wants everywhere else. A sky at the far plane has to pass against a
depth image that still holds its clear of zero, and greater-than alone rejects it
everywhere.

**The sky draws between the opaque draws and the blended ones**, which closed issue #435.
`MeshPass::draw` is `draw_opaque` and `draw_blended` now, and `SceneRenderer` puts the sky
between them. The order stays in `SceneRenderer`, where the rest of the frame order lives,
rather than moving the sky inside `MeshPass`.

**The bug was worse than "blends against the clear colour".** A blended surface writes no
depth, so the depth image over that surface still holds its clear. The sky tests depth for
equality and passes there, and the sky is opaque. So a pane over open sky was not tinted
wrongly. It was painted over and gone.

**The split needed one thing the single call did not.** `draw_opaque` binds the frame set,
and `draw_blended` used to inherit it. `SkyPass` binds a set of its own against a layout of
its own, and Vulkan invalidates every set bound against an incompatible layout. So
`draw_blended` binds the frame set again. The validation layer reported it on the first
frame of the new scene: "uses set #0 but that set is not bound".

**The frame report takes the sky off the mesh number.** The sky range is inside the mesh
range now, so reading the mesh pair alone would report both. `read_timestamps` subtracts it,
and "mesh" still means the cost of the geometry.

**`tests/content_sky/` is the scene that proves it.** Nothing else here has both halves: the
sandbox is a closed room, Sponza carries no blended geometry, and `tests/content` names no
environment on purpose. It is one blended pane over the left of a frame of open sky, and the
environment is a vertical gradient and nothing else, so the two outer thirds of a frame of
sky alone are the same picture. `test_gpu_frame` compares them. The pane moves two channels
by about 44 of 255, and the sky painting over it gives under 1.

### M6 — moth_ui spike, 2 to 3 days, timeboxed
Write a minimal `IRenderer`, `IImage`, and `IFont` against `gfx::`. Render one static layout
with an image and a string. **The purpose is diagnostic, not feature work.** It tells you
whether an external consumer can use `gfx::`, while the interface still costs little to
change. Fix what the spike exposes, then stop.

We own both sides, so a finding can land in `gfx::` or in moth_ui. That makes the fix cheaper
and the report easier to fake, because you can patch whichever side is convenient and learn
nothing. Name the side each fix landed on. See §8.

**Done when:** one moth_ui layout draws in the engine, and you have written down what the
spike taught you about `gfx::` and about moth_ui.

**M6 is complete.** `sandbox/content/ui/main.mothui` draws an image and two strings, and
`--offscreen` captures it repeatably. The findings are in §8.6, with the side each fix landed
on. The short version: `gfx::` came through with one real gap, the sampler in #209, and rule
4.1 and rule 4.2 both held. The interfaces that turned out to be wrong were moth_ui's, and
they are wrong in one way: they assume a game reads loose files from a disk. None of them was
fixed here, so no moth_ui release was needed.

The spike stayed inside its timebox. The definition says 2 to 3 days. The M6 issues were
filed on 2026-08-10, the first M6 commit landed the same day, and M6.5 closed on 2026-08-11.
That is about one day.

Text was still half the work, which §8.6 measures. It fit because `moth_graphics` had already
solved the hard part and the port was mostly the texture upload, which is exactly what §8.3
predicted.

### M7 — Physics
Connect Box3D to enkiTS. Add rigid body and collider components. Reflect them, so the
inspector needs no extra work. Add debug draw. Add a fixed timestep with render
interpolation.
**Done when:** you hit a stack of boxes and it falls.

**M7 is complete.** A stack of three crates stands in the sandbox room, and a crate thrown
from the camera knocks it over. `--key <frame>:throw` fires the same throw on a fixed frame, so
the picture that proves it is one a person can produce again.

It arrived in six parts. M7.1 vendored Box3D and confirmed it agrees about which way is up.
M7.2 put its task callbacks on enkiTS. M7.3 added the reflected `RigidBody` and the colliders,
and moved the transforms both ways, which needed M7.3a to describe an enum. M7.4 added
`engine::FixedTimestep` and the pose blend. M7.5 read the wireframe out of Box3D and drew it.
M7.6 built the stack and threw something at it.

**The simulation is deterministic, and that was measured rather than assumed.** Three offscreen
runs of the same command, with the solver split over eight job system workers and a crate
thrown on a fixed frame, produce images equal byte for byte. §9 records why that matters: a
later replay and a later network layer both rest on it.

The physics step costs 0.004 ms of a 3.5 ms frame in the sandbox, against a mesh pass of 1.50
ms. That is five bodies, so it measures the overhead rather than the solver. §5.1 holds the
measurement that does load it.

Two things the milestone did not do, and both are now done. #237 closed after the milestone. A
collider reads the scale on its entity now, and `sandbox/content/main.scene` stands a crate at
1.6 beside the stack to show it. #245 closed after that. The game logic runs on the fixed step,
and `scene::StepMotion` blends what it moves for the frame. The §9 note holds both.

### M8 — Scripting
sol2. A `ScriptComponent` with `on_start`, `on_update`, `on_destroy`, `on_trigger`, and
`on_contact`. Reflection-driven
binding with a **curated** surface. A fully mechanical binding produces an API that nobody
enjoys. Add hot reload from the start.
**Done when:** the sandbox game logic runs entirely in Lua.

Seven increments, #259 to #265.

**Input comes first, and it is not scripting.** §6 lists input under `platform/`, and the
directory has never held it. Every input read in the engine today sits inline in
`apps/runtime/main.cpp`. A script API over that inherits an SDL shape, which is what #207
warns about. So M8.0 builds the module and the rest of the milestone binds it. M10 puts the
moth_ui bridge on the same module.

**The binding has two halves, and neither one can borrow the method of the other.** A
component field goes through the reflection descriptors. The alternative is a hand-written
binding for each component, and a game type like `sandbox::Spin` could never reach that. So
the first half is mechanical, and nobody picks the fields by hand.

Everything else stays hand-written and small: the world, prefabs, physics, input, math, and
logging. Generating that half from the descriptors gives the API nobody enjoys. So a person
chooses the second half one call at a time, and rule 4.6 keeps it to what the sandbox needs.

The §7 note predicted this split. `reflect::Registry` records facts about a type and cannot
act on one. #25 closed on the reasoning that the right operation set needs a second caller to
state it. The script binding is that caller.

**A script is an ordinary asset, and one cooked form serves both uses.** The cooker has no
`.lua` rule, so a `.lua` already reaches the copy path, takes a GUID, gets a `.meta` sidecar,
and enters the manifest. M8.1 makes that deliberate. A person edits the `.lua` in the source
tree, the M4.5 watcher cooks it again, and `Content::reload` names that one asset. So the
development loop and the packaged game read one path, and no build mode decides which script
ran.

The cooked form is the source text, and the reason is weaker than it looks. Lua writes the
instruction size, the integer size, the number size, and the byte order into the header of a
binary chunk, and the loader refuses a chunk that disagrees.

Both engine targets are x86-64 and little-endian, and both build Lua the same way. So those
values match, and a chunk cooked on Linux does load in the Windows runtime today. The format
promises nothing about that. A third target, a 32-bit build, or a Lua major version each
break it, and the failure arrives at load time rather than at cook time.

Bytecode also buys nothing measurable for a handful of small scripts. That is the reason M8
skips it, and the portability is only why it stays skipped by default later.
`luaL_loadbuffer` takes source or bytecode through one call, so a later precompile step
changes the cooker alone. #258 holds it.

**A reload restarts the script, and the script table is thrown away.** M8.5 settles this. The
next fixed step runs `on_destroy` on the old instance, builds a fresh environment, and runs
`on_start` on it. So a reload is a destroy and a create, which is one sentence to explain and
one path in the code: the sync that already handles an entity changing which script it names
handles this too.

Carrying the table across was the alternative, and it has no answer for a value whose shape
changed between the two versions. The wrong answer there is a bug that reads as a game bug, in
the middle of the debugging session the reload existed to help. Throwing it away is wrong in an
obvious way instead of a subtle one.

**So state that has to survive belongs on a component, not in the script table.** A script
reaches any reflected component with `entity:get` and `entity:set`, which M8.2 built. A reload
rebuilds the Lua instance and no entity, so component state carries across untouched. The rule
is therefore: the script table is scratch, and a component is storage.

That rule is worth holding to for its own sake. State on a component is saved by the scene
file, shown in the inspector, and reachable from C++. State in a script table is none of those
things.

**A text that will not compile changes nothing.** The old text keeps running and the message
names the file and the line, so a save in the middle of an edit cannot take the game down. This
is the pattern `MeshPass::reload_shaders` already uses for a pipeline that will not build.

An instance an error had stopped is restarted by a reload as well. Without that, fixing a
script and saving it would leave the entity dead until a scene reload, which nobody would
predict.

**The fixed step is the constraint the whole milestone works under.** #245 put the game logic
on it, and M7 proved three offscreen runs byte-identical. Lua can lose that in two ways, and
the two need different answers.

`math.random` seeded from the clock gives a different sequence on each run. M8.3 seeds it
from a fixed value, and that settles it.

`pairs` costs more. Lua 5.4 seeds its string hash from the clock and from an address, so the
walk order of a string-keyed table changes between two runs of one binary. The array part
stays ordered, so `ipairs` and an integer-keyed table are already safe. Making `pairs` itself
deterministic means patching `luai_makeseed`, and rule 4.4 then turns Lua into a vendored
dependency. So the cheap answer is to keep an ordered walk out of `pairs`, and M8.3 writes
the rule down.

Neither one announces itself. The first symptom is a determinism test that fails once in ten
runs.

**The milestone needs two things from physics that M7 did not build.** A trigger volume and a
contact event. The puzzle asks whether the stack landed in the goal, and nothing in the engine
can answer that today. M8.4 builds both, and §5 gains whatever Box3D costs to read.

**The two events reach a script differently, because they are not the same shape.** A trigger
has a direction: one side is the volume and the other crossed it. So `on_trigger(other, began)`
runs on the volume alone, and `other` is the visitor. A goal asks what landed in it, and a
crate never has to know what a goal is. A contact has no direction that Box3D promises, so
`on_contact(other, began)` runs on both bodies, each with the other as `other`. A one-sided
call there would land on whichever body the solver listed first, and which script heard a
collision would depend on solver internals.

**The delivery runs inside the fixed step, right after the solver.** The simulation keeps the
events of one step only, so a frame that ran three steps and read them once would report the
third and throw the first two away. That would lose the events a puzzle cares about, and it
would put the result back on the frame rate that #245 removed it from.

**M8.6 closed the milestone, and two engine gaps turned up while authoring it.** A script had
no way to reach the camera, and the throw is "throw where I am looking". And a prefab a script
instanced carried its collider components and no body, because the simulation reads the world
once at build and does not scan for new entities each step. So `script::CameraView` joins the
services and `entity:add_body()` joins the curated surface.

The reset needed a third. **A dynamic body owns its pose, so writing an entity transform does
nothing to it**: the next step overwrites whatever the caller put there. `Simulation::teleport`
is the only way to move one, and it stops the body dead, wakes it, and sets both halves of the
pair `interpolate()` blends. Setting only the newer half draws the body sliding from where it
used to be, across everything in between.

**Destroying an entity from a script found a fourth.** The simulation held a body for an entity
that no longer existed, and the next step read the RigidBody off it and died on an EnTT
assertion. `Simulation::step` drops a body whose entity has gone before it reads any of them.
Nothing did this while the game was C++, because nothing destroyed a body-carrying entity while
the world was running.

**Input edges and the fixed step are on different clocks, and that is not a rare race.** A
press edge is the difference between two `Input::update` calls. The camera runs on the frame
and the game runs on the fixed step, and a frame often runs no step at all. Offscreen it almost
never does, so an edge raised and cleared between two steps is one the game never sees. The
runtime therefore folds every device frame into what the next step reads, and keeps a second
`platform::Input` on the step clock. One Input for both cannot work.

**The done-when test is authoring, not porting.** The sandbox game logic today is `Spin` at 30
lines, plus a crate throw that lives in the application rather than in the game. Moving those
two proves the binding is complete. It does not prove the binding is pleasant, because a port
already knows the answer it wants. So M8.6 also authors the physics puzzle §11 names, in Lua
first and never in C++: a goal volume, a win, and a reset.

The test moved with it. `sandbox::update` and `throw_crate` are both gone, so
`tests/test_sandbox.cpp` drives the puzzle through a cooked `.lua` and the script callbacks.
There is no C++ entry point left for it to call, which is the point: a test that kept one alive
would pass while the binding did nothing.

### M9 — Editor split
`editor` and `runtime` as separate executables over `engine_core`. The game module links
into both, so the editor holds the project types and can inspect them. A release build
compiles with `WITH_EDITOR` off and drops the editor code. See rule 4.3.

Play-in-editor through world snapshot and restore, which M2 and M3 already provide.
ImGuizmo. An asset browser, a hierarchy panel, and an inspector panel.

**The scene owns the camera it plays through, and the editor gets a separate one.** Both
were one struct until M9.5: `editor::ViewSettings` held the only camera, both applications
read it, and it lived in a file beside the executable. A level whose viewpoint lives there
is not a level anybody can ship, which is what M9.8 asks for.

M9.5a is the first half and it has landed. `scene::Camera` is a reflected component, so a
level carries its viewpoint the way it carries its lights, and a script that acts along the
line of sight reads the camera the game plays through. Both applications draw through it.

M9.5b is the second half and it has landed. The editor has a free-fly view that belongs to
the person rather than to the scene, saved beside `imgui.ini` rather than in the project.
The viewport keeps showing that view while a session plays, so a person can fly around a
running game while the game keeps playing through its own camera.

**The editor never writes to the camera entity.** It reads that entity for one thing only:
the pose a script sees, through `play::View`. So flying in the editor cannot move a level's
camera, and a scene saved after an afternoon of flying is the scene somebody authored.

The exposure the editor tonemaps with is the scene camera's, not the editor's. Exposure is a
property of the level that a person judges by eye, so the viewport has to show what the game
will show.

**The editor draws the scene camera as a wireframe**, because the editor view is not that
camera and nothing else says where it is. `editor::camera_lines` builds a pyramid from the
camera out to a fixed length, with a bar over the top so a person can tell which way up it
is. A symmetrical pyramid reads the same rolled by any angle.

The width of that pyramid is the aspect the editor is drawing at rather than one the camera
holds. A `scene::Camera` records a vertical field of view and nothing about the window a
game will run in, so the height is exact and the width is a guess. Issue #327 holds the
option that would let a scene say.

**Clicking an entity selects it**, which closed issue #34 after it had been open since M3.3.
`math/ray.h` turns a point on the picture into a world ray, and `editor::pick_entity` finds
the nearest entity that ray meets. Empty space clears the selection.

The ray goes into the local space of each candidate rather than the bounds coming out into
the world. That makes the test an oriented box for the price of one inverse, and a sphere
would have been wrong for the shapes an editor holds: a sphere around a long thin wall is
mostly empty space, and clicking that space would select the wall.

**A ray that starts inside a box reports where it leaves, not zero.** Every scene the editor
opens is a room: a box that contains the camera and everything in it. A hit at zero beats
every object inside, so clicking anything selected the room. Reporting the far side puts the
room behind its own contents, which is where a person sees it, and the room is still
selectable wherever nothing else is in the way.

**A click the gizmo owns does not change the selection.** The overlay draws the handles
first and picks only when ImGuizmo reports neither a hover nor a drag, so grabbing an arrow
cannot select whatever sits behind it, and a drag that wanders off the handle keeps the
entity it started on.

The bounds arrive through a callback, so `src/editor/` needs nothing from `src/render/` and
a test drives the search with bounds it makes up. The editor answers that callback from
`render::MeshCache`, which already holds every mesh the scene drew.

**The editor cooks after it saves.** It writes the source scene, in `sandbox/content`, and it
reads the cooked one, beside the executable. Between those sits the cooker, and until M9.8
found it the editor never ran one, so every edit looked lost on the next start: the file on
disk was right and the program read a different file. Nothing reported an error, because
nothing had failed.

The runtime does not have this problem, because `assets::HotReload` watches the source tree
and cooks what changes. Giving the editor a watcher is the larger answer and it wants the
entity identity of M12 first, because a reload rebuilds every entity and throws away the
selection and the undo history with it.

**M9.5c puts ImGuizmo on the selected entity.** Move, turn, and size, in world space or
in the entity's own. The handles write through `World::set_local`, so every child follows
a dragged parent, and a child dragged under a moved parent lands where the pointer is
because the parent is divided out first.

**The gizmo modes are on W, E and R**, which closed issue #325. Every editor binds those
three keys, and this one could not while the fly camera moved on them whenever they were
held.

**The camera is what changed, not the keys.** `FlyCamera::move_needs_look` moves the camera
only while the look button is down, which is what Unity does and what frees the letters the
rest of the time. The editor turns it on and the runtime leaves it off: the runtime has flown
with WASD alone since M2, there is nothing else those keys could mean in a game, and a debug
camera that needs two hands is a worse debug camera.

**The rule is the button, not the panel.** `editor::apply_gizmo_shortcut` ignores the keys
whenever the right mouse button is down. A rule that also asked which panel the pointer was
over would be more precise and harder to hold in the head, and the imprecision costs nothing:
the button is down only while somebody is flying.

The decision lives in `src/editor/panels.h` and the key reading lives in the application, so
`tests/test_editor.cpp` drives it with no window.

**ImGuizmo needs a projection this engine never builds.** The engine renders with an
infinite reverse-Z projection whose Y row is negated for Vulkan clip space. ImGuizmo does
its own clip to screen step and expects the neutral form: Y up, and a finite far plane. So
`apps::gizmo_projection` builds one for the handles alone, and the picture keeps the engine
matrix. The two agree about where a point lands, because the X row is the same either way
and the two Y flips cancel.

The editor runs the ImGui docking branch, which `conanfile.py` already pins for this
reason. Panels dock and tab inside the main window, and a panel dragged out of it becomes
an OS window of its own. M9.7 turned that on, and the backends did most of it as expected:
ImGui creates the extra SDL windows and their swapchains, so `platform::Window` keeps its one
window and `gfx::Device` keeps its one surface.

Two things did not come free. **A detached panel needs its own pipeline format**, and
`ImGui_ImplVulkan_InitInfo::PipelineInfoForViewports` is both where that is stated and what
the backend asks the new surface for, so naming the main swapchain format there is what keeps
a detached panel on the same format rather than on whatever the surface offered first. And
**the colour conversion has to reach them**. The overlay converts every vertex colour from
sRGB to linear because the swapchain is `_SRGB`, and the extra swapchains are too, so a panel
dragged out would come out lighter than the one it was dragged from. `imgui_render_platform_windows`
converts each extra viewport's draw data before it presents them.

**`editor --own-windows` is how the path is checked.** Nothing else can drag a panel out, so
that flag turns ImGui's viewport merging off and every floating panel becomes an OS window at
once. With it, the editor runs three platform viewports, two of them real windows with
swapchains of their own, and `--sync-validation` reports nothing over 300 frames.

Docking and the saved layout are chosen per application, not in `gfx::`. The editor wants
both. The runtime debug overlay wants neither, and its windows open at fixed places so a
run always starts from the same layout. `gfx::ImGuiDesc` carries the two answers.

The editor writes its `imgui.ini` under `platform::preferences_directory`, which is where
the platform puts the settings of one user. Nothing goes beside the executable, because an
installed program usually sits where the user cannot write.

ImGuizmo has no package the editor can take. Every recipe on Conan Center pins
`imgui/1.90.5`, which conflicts with the docking branch, so M9.1 took the requirement out
to make the option resolve. Issue #308 holds the two ways out, and M9.5 needs one of them.

**Done when:** you build a level in the editor, press play, and ship it as a runtime build.

### M10 — Game UI
Complete the moth_ui integration on the M6 foundation. Add widgets when `sandbox/` needs them.
**Done when:** the sandbox game has a main menu, a pause menu, and a HUD. You author them in
`moth_editor` and they hot-reload.

**M6 already drew a layout, so M10 starts further along than this section used to say.** The
spike built the batching recorder in M6.2, and the font atlas and text rendering in M6.4.
`src/ui/renderer.h` holds the recorder and `src/ui/font.cpp` holds the text. An earlier version
of this section listed all three as M10 work, and the GitHub milestone copied it. Neither was
true after 2026-08-11. What M6 left is one layout that the runtime asks for by hand.

So M10 is the four things the spike did not build:

- **Layouts as cooked assets, with hot reload.** §8.4 names it and issue #211 holds it. M13
  changed what an asset type has to do, because a rule now writes through `import::Writer` and
  the editor imports the same bytes the cooker writes. A new cooked type needs both halves.
- **The identity a layout stores.** M6.3 resolved an image path against the cooked manifest and
  §8.4 deferred the decision to this milestone. **The decision is made: moth_ui takes an asset
  identity, and the path stops being the identity.** The reasoning is below.
- **The input bridge.** SDL3 events become moth_ui events. Controller navigation lives at this
  seam.
- **The Lua bindings.** A script drives a menu, which is the right way to author UI behavior.

**The identity change is narrower than §8.4 estimated.** That section expected a change across
moth_ui, moth_graphics and moth_editor together, and said so during a spike it wanted kept
short. Reading the call sites gives a smaller number. `IImageFactory::GetImage` has one caller
in moth_ui, at `src/nodes/node_image.cpp`, and one implementation in moth_graphics. moth_editor
calls none of them, because `NodeImage::GetImage` is a different method that returns the image
a node already holds. The rest are mocks and API-surface tests.

**moth_editor is still touched, through the write path rather than the read path.**
`editor_panel_canvas.cpp` builds a `LayoutEntityImage` from a drag-drop path, so moth_editor is
where an image reference is created. A first count of this read only `GetImage` and missed it.
So the change reaches three repositories and the engine. That is fewer than §8.4 estimated and
more than a count of the read path alone suggests.

**Whoever creates a reference now owns normalizing it.** moth_ui stops calling
`std::filesystem::absolute` and `std::filesystem::relative`, so it no longer turns a path into a
project-relative one on the way through. moth_editor has to do that before it makes the entity.
Otherwise a layout authored there stores an absolute path and stops working on another machine.
That failure compiles, and it draws correctly on the machine that authored it. So the identity
type takes explicit construction, and the compiler names every site that has to be read again.

**A path must stay a valid identity**, because moth_graphics and the VanishingPoint game both
name an image that way and neither reads a cooked tree. So the identity is a type a path
converts into, rather than a type that replaces a path. `LayoutEntityImage::Deserialize` also
stops calling `std::filesystem::absolute` on what it read, which closes #218 and takes
`engine::ui::source_path_for` away.

**The engine pins moth_ui, so this milestone releases moth_ui and moves the pin.** §8.5 says a
Conan editable is for development and not for a build somebody else runs. Develop the two
repositories together against the editable, then release and pin.

**M10 is complete.** M10.7 built the milestone test, and the authoring pass is recorded below.
The sandbox game has a main menu, a pause menu and a HUD. `scripts/puzzle.lua` moves between the
three, and no C++ knows any of them exists.

**The milestone test alone did not close it.** The pass found four gaps, and every one of them
went into the milestone as M10.7c through M10.7f rather than beside it. None blocked the test
above, and each is a thing a person meets while authoring a menu rather than a thing they read
about. All four are answered.

It came in three parts. M10.7a gave a script the fixed step through `script::GameClock`, because
a pause menu that cannot stop the world is a picture rather than a menu. #402 gave a script a
node inside one particular reference, because three menus of buttons is exactly the shape a bare
node id cannot name. #396 gave the runtime a click it can replay, because a menu that answers a
click cannot be proved in a capture without one.

**A run starts at the main menu with the game held**, so the capture with no arguments is the
title screen over a world that has not moved. `--click 5:640,382` presses Play. `--click` is
repeatable and it replaced the single `--click-at-frame` and `--click-at` pair from #396: one
click can never walk a game through its own screens, and the pause menu takes two.

**The pause takes the key away from the game, and that is measured rather than argued.** A run
that starts the game, pauses it, and then holds the throw key down on frame 80 is byte for byte
the run that never pressed it. The same key on a running game gives a different picture, so the
gate is a gate rather than an input path that stopped answering.

**Which screen is up is the UI's own state, and no component records it.** A reload keeps every
layout showing and keeps the session paused, so `on_start` puts the main menu up only when
nothing is up yet. That falls out of two rules M10.6 and M10.7a already settled, and it is worth
naming because the obvious answer is a reflected component that would then disagree with the
surface.

**A build with no game UI plays without a menu.** Only the runtime binds a surface, so a game
played in the editor viewport has none, and neither has a `with_ui=False` build. A game that
paused itself with no menu on the screen could never be started, because only a button can
resume one. `tests/test_editor.cpp` is what holds this: it drives the shipped scene with no
surface, and it fails if the world stands still.

### What the M10.7 authoring pass found

Four gaps, all filed, none of them fixed on that branch. M9.8 found nothing and said so, and this
pass is the other outcome: authoring the thing is what finds what building the parts cannot.

- **#410, M10.7c, a script is not told when a layout reloaded. Answered.** A reload builds the
  node tree again, so every label a script wrote went back to the text the file carries, and
  `on_start` runs again only when the **script** reloads. So the first save while authoring a
  menu made every button in it read `Button`. `on_ui_reload(layout)` is the answer and §8.4 holds
  it. The mutation is the whole of the proof: take the callback out of `scripts/puzzle.lua`,
  edit the three layouts mid-run, and every label reads `Button` again.
- **#408, M10.7d, a paused game reads no key. Answered.** A script reads an action inside
  `on_update`, and a paused session runs none, so P put the pause menu up and only the Resume
  button took it down. `on_paused_update()` is the answer and §8.4 holds it. P resumes now, and
  the capture proves it through `--key 90:pause`.
- **#409, M10.7e, a paused game that moves something does not redraw it. Answered.** The Main
  menu button puts the room back, and the crates stayed drawn where they fell until something
  resumed. A paused advance interpolates now, and §8.4 holds the two halves it needed.
- **#407, M10.7f, Escape quits the runtime. Answered.** So the pause menu was bound to P, which
  is not the key a player reaches for. Freeing Escape needed another way out of the runtime
  first, and it is the Quit button of the game's own main menu, through `game.quit()` and
  `script::GameExit`. §9 holds the rule about which keys belong to whom. It was the last of the
  four for that reason: the other three had to settle before the key could move.

**Three of the four are consequences of the pause, and none of them showed up while it was
built.** M10.7a has four cases and three mutations behind it, and every one of them is about a
session in isolation. What they cannot ask is what a game does with a paused session, and that
is the question the pass answered.

### M11 — Audio
miniaudio behind `IAudioDevice`. Positional 3D and buses.
**Done when:** the sandbox game plays sound.

**The milestone starts from an option and nothing else.** `conanfile.py` already carries
`with_audio` and asks for `miniaudio/0.11.22`, and Conan already writes `ENGINE_WITH_AUDIO`
into the toolchain. No `CMakeLists.txt` reads that variable, and `src/audio/` does not exist.
So every part above the option is M11 work. §6 already places the directory and §5 already
records the package.

**miniaudio is contained the way Vulkan and Box3D are.** Rule 1 keeps `vulkan.h` under
`src/gfx/vulkan/`, and `scripts/check-box3d-containment.sh` keeps Box3D inside `src/physics/`.
`miniaudio.h` gets the same treatment, checked by a script in the `containment` job. §5 rejects
an audio plugin ABI, so `IAudioDevice` exists to hold the containment line rather than to carry
a second backend.

**The cooker links miniaudio's decoders, and that is a change #424 made on purpose.**
`src/import/sound.cpp` decodes a short effect once at cook time so that nothing decodes while
it plays. Only WAV did before, so a project keeping its effects compressed got none of the
benefit of the PCM path.

The decoders and the device layer are separable, and this uses that.
`src/audio/miniaudio_config.h` defines `MA_NO_DEVICE_IO` when `with_audio` is off, so a build
with no audio carries the decoders and no backend. An import still opens no device and still
runs on a build machine with no sound card, and a cook produces the same tree byte for byte
whatever the option says. That last part is checked by cooking `sandbox/content` with both
builds and comparing.

**There is one miniaudio implementation for each binary, in `engine_miniaudio`.** miniaudio
carries its own implementation and exactly one translation unit may ask for it. Two targets
need it now, `engine_core` to play and `engine_import` to cook, and `apps/editor` links both.
Two copies would be duplicate symbols at best, and at worst two struct layouts that disagree
about a macro, which links cleanly and goes wrong at run time.

**Which decoder reads a file is decided by its bytes and never by its name.** WAV keeps the
hand-written reader in `import/sound.cpp`, because it already exists, it is tested, and moving
it would change the bytes every cooked WAV already has. FLAC and MP3 go through miniaudio.

**miniaudio 0.11 has no Vorbis decoder**, which the M11 work never met because no real `.ogg`
has been through this engine. It carries dr_wav, dr_flac and dr_mp3, and Vorbis needs a
`ma_decoding_backend_vtable` the caller supplies. So an `.ogg` is accepted, cooks as a streamed
sound, and cannot be played. Issue #477 holds it, and the fix has to reach the mixer and the
cook path together or a file cooks and stays silent.

**A machine with no sound card must still run.** A CI runner has none, and an offscreen capture
runs on machines that have none either. So a device that cannot open hardware opens silent and
reports it, rather than failing the program. Every test runs on that path, which makes it the
first thing to build.

**Audio never reaches the fixed step.** The mixer runs on its own thread and a game reads
nothing back from it that decides what to simulate. Three offscreen runs of one command already
have to give one image, and M11 must not move that. A sound is an output of the simulation and
never an input.

**A sound is cooked two ways, and the sidecar picks.** A short effect cooks to raw PCM at one
sample rate, so nothing decodes on the load path. A long track keeps its encoded bytes and
streams. miniaudio decodes both, so the cost is one field in the `.meta` and two paths in the
cache. Cooking everything to PCM was considered, and it makes a music track cost tens of
megabytes in the cooked tree for no gain.

**The rule writes through `import::Writer`.** M13 made that a requirement of every asset type:
the editor imports a source file through the same rules the cooker runs, and the two must agree
byte for byte. A new type that skips the import half works in the runtime and not in the editor.

**The sandbox sounds are generated, not fetched.** A script synthesizes a click, a thud and a
tone into WAV files, the way the room and the sphere models were generated. That leaves no
license question to answer and keeps the files small. **The generator stays in `scripts/`**,
unlike the model and layout generators, because nobody can author a WAV by hand and a sound
that is wrong has to be made again.

**The listener is a reflected component**, the way `scene::Camera` is, and it falls back to the
primary camera when a scene names none. A scene says where the ears are, and the editor flying
its own camera must not move them.

**A voice a script starts belongs to that script.** M8.5 settled that a reload restarts a
script and the script table goes with it, because carrying a table across two versions has no
answer for a value whose shape changed. A voice is the same kind of thing: scratch, not
storage. So the host stops every voice an instance started when that instance goes, whether it
went to a reload, a changed script, or a destroyed entity. Otherwise a looping sound plays on
with nothing left that holds its number, and only restarting the game would quiet it.

A sound that has to survive a reload belongs on a `scene::AudioSource`, which is a component
and therefore storage. That is the same split M8.5 drew.

**The editor plays a session's sound, and the ears are the scene's.** A session in the editor
is the game, so it hears what the game hears: `scene::AudioListener` if the scene names one and
`scene::primary_camera` otherwise. The editor's own fly camera is never the listener.

That is a choice with a cost, and the cost is worth naming. A person can fly the editor camera
across the level while a session runs, and they then hear a place they are not looking at. The
alternative costs more. Making the editor camera the listener would mean a level's own listener
placement is never heard until the game ships, and it would break the property M11.4 built:
that flying the editor camera cannot move the ears. One behaviour that is occasionally
surprising beats two behaviours that disagree.

**In Edit state the editor is silent.** Nothing is wired until Play, so an authored scene makes
no sound and a person hears only what they asked to hear. Stop silences everything the session
started, a looping voice included, by two paths: the host stops what each script instance
started as it destroys that instance, and `PlayMode::silence` stops what the scene's components
started. Both are needed, and deleting either one fails `tests/test_editor.cpp`.

**Nothing in the script surface reports what the mixer is doing.** A script starts a voice and
stops it, and it cannot ask whether one is still playing. That answer comes off another thread
against the real clock, and a game that read it would stop being reproducible. Issue #245 took
the wall clock away from `on_update` for that reason, and this is the same rule.

The increments, in build order:

| Increment | What it builds |
|---|---|
| M11.1 | The device. `src/audio/`, containment, `ENGINE_WITH_AUDIO`, and the silent device |
| M11.2 | The sound asset. The cooked format, the cooker rule, and the import half |
| M11.3 | Playing one. The cache, a one-shot, and a bus to play it on |
| M11.4 | `scene::AudioSource` and `scene::AudioListener`, and positional 3D |
| M11.5 | Buses and volumes, reflected and saved |
| M11.6 | The Lua surface, and a reload that leaks no voices |
| M11.7 | The milestone test. The sandbox game plays sound |

**M11 is complete.** The sandbox game plays sound, and every sound it plays is authored in Lua
or in a scene rather than in C++. A button clicks, a thrown crate is heard leaving the camera,
a crate landing thuds where it landed, a reset answers the player, and a pause quiets the room
without silencing the menu that is doing the pausing.

**The counts are the evidence a run leaves behind.** `runtime --offscreen` reports what the
audio did, and the report is what turns "I hear nothing" into an answer:

| What the run did | Voices | Sounds loaded |
|---|---|---|
| Nothing. The title screen, paused | 0 | 0 |
| Play clicked | 1 | 1 |
| Play, then a throw | 7 | 3 |
| Play, a throw, then a reset | 8 | 4 |

A paused game plays nothing and loads nothing, which is the row that makes the rest of the
table mean something. **Three runs of the same command give one image and the same voice
count**, so the sound follows the simulation and nothing about it feeds back.

**A crate landing needed a script of its own.** `puzzle.lua` runs on the goal volume, and a
trigger never hears a contact: an overlap reaches the volume and a contact reaches the two
bodies that touched. So `crate_sound.lua` runs on the crate prefab. That is the first time this
game has had two scripts, and it is the right shape here for the reason M8 gave for one: the
throw, the win and the reset share state, and a thud shares none.

**A thud for every contact is a burst rather than an impact.** A crate that lands touches
several times over as it settles, and a stack touches every neighbour. `crate_sound.lua` holds
a cooldown in simulated seconds and reads the body's speed, so a crate the player threw is loud,
a crate settling is quiet, and a crate creeping is silent.

**The authoring pass found one real bug, and it was in the engine rather than in the game.**
The runtime pushed the saved volumes onto the mixer on every frame, so the panel was the only
writer that lasted. A script that muted the effects bus to quiet a paused room was overwritten
on the next frame, and the pause did nothing at all. `apply_mix` compares against what it last
applied now, in both applications, so a slider still reaches the mixer at once and a bus nobody
touched is left where the game put it.

That is the class of bug an authoring pass exists to find. Every part of it worked on its own:
M11.5 set a bus and proved it, M11.6 let a script set one and proved that too. What neither
could see is that two writers disagreed, because each test held only one of them.

**The pause rule is checked by a run rather than by a test**, and that is a gap worth naming.
`tests/test_sandbox.cpp` binds no UI surface, so `pause_game` in puzzle.lua returns before it
pauses: a pause with no menu on the screen would be a game nobody could resume, which M10
settled. So the test checks the half it can, that the world plays on the effects bus, and the
runtime's audio report is where the mute is read.

**The pause rule and the four sounds are settled, on 2026-08-22.** Somebody listened to the
set and accepted it. The rule stays as M11.7 built it: a pause mutes the effects bus and
leaves the menu audible on the master. A retune is an edit to `scripts/make-sounds.py` and a
re-run, and nothing about that choice needs asking again.

**Nobody heard any of it until the end.** Six increments were built and merged on the strength
of sample values, channel comparisons and voice counts, all of them on a silent device. The
first listen was a person pressing V on a real machine, and the click was there. The numbers
were telling the truth, and they could not have said so.

### M12 — Editor undo
Every edit the editor makes can be undone and redone. The editor changes a world in place
and keeps no history today, so a gizmo drag, a component added or taken off, a deleted
entity, and a dropped prefab are all final. Reloading the scene is the only way back, and it
throws away everything since the last save rather than the last thing.

**Transactional. Each edit records what it changed and how to put it back**, so a step costs
what the edit cost rather than what the level costs.

A stack of whole-world documents was considered and rejected. It is far less code, and
`scene::save_scene` already writes one while `editor::PlayMode` already restores a world from
one. The measurement that made it look cheap was taken on the sandbox scene, which is 43
entities on purpose: 7.2 KiB and 0.62 ms for a step. That number says nothing about a level
worth building. At a few thousand entities every click writes hundreds of kilobytes, and
undo gets slower as the level gets better, which is the wrong way round.

**Transactional does not mean nothing is serialized.** Deleting an entity has to keep the
subtree it removed, because there is no other way to bring one back. It means the unit is
what changed. The pieces are already here: `ComponentOps::save` and `load` give a component
as a document, and `scene::diff` writes the merge patch between two documents.

**What a delete keeps is a fragment**, which `scene::save_subtree` writes and
`scene::load_subtree` reads. It holds the entity records a scene holds, and two more keys
saying where the subtree hung: the identity of its parent, and the identity of the sibling it
sat in front of. The second one is what a weaker answer drops. A subtree brought back at the
end of its sibling list holds every entity and writes a different file, and child order is
what a scene file writes and what the hierarchy panel shows. `World::set_parent` takes a
sibling to sit in front of for that reason.

A fragment also carries one thing a scene file never writes: the link from an entity to the
prefab instance it belongs to. A scene collapses an instance to one record, so no member ever
gets a record of its own there. A fragment can be one member somebody deleted, and then
nothing rebuilds the link. Putting the member back with that link is what takes the instance's
`removed` list away again, so the instance is an instance rather than a hole and a loose
entity.

A root of the world keeps no place, because the roots come out sorted by entity value and
EnTT hands a slot number out again. The entity, its identity and its data all come back. Only
the order of the records moves. Issue #353 holds it.

Two things do not fall out of the shape.

**Every entity carries a stable identity, and an edit names one through that.** An
`entt::entity` is a slot number that EnTT hands out again, so nothing outliving one edit can
use one: not an undo entry, not the selection, and not an entity naming another entity.
Undoing a delete builds the entity again, and a stop rebuilds the whole world, and both used
to leave every entry on the stack naming whoever took its number.

`engine::Guid` is the tool, unchanged: `generate` for a new one, `derive(parent, kind,
index)` for a part of something, and a text form for the file. **A prefab member derives its
identity from the instance root**, the same way the cooker derives a mesh identity from the
glTF that holds it, because an instance is one record in the file and storing an identity for
each member would bloat every one. The scene file goes to version 4, and a version 3 document
still reads with an identity made for each entity as it loads.

**An entity somebody added under an instance keeps a stored identity, not a derived one.** It
is scene data rather than prefab data, so its record already carries its parent and its
components, and one more field costs nothing. A derived identity would be stable from one read
to the next but would change on the **first** save, because the index it derives from is only
worked out when the file is written. That would break every undo entry naming an entity the
author had just dropped into an instance, which is the case the identity exists for. The root
is the same shape: it is a member of its own instance at index 0 and it derives nothing,
because deriving over it would throw away the identity the record just gave it and move every
member with it.

Two things fall out of it. A **delete is a real delete**, because undo builds the entity again
with its identity and every other entry still resolves; an earlier plan to keep deleted
entities alive and hidden is not needed. And **the history lives through a play and a stop**,
which is the loop this milestone is for, because the rebuilt world carries the same identities
out of the same document.

**The play and stop loop cost nothing to build.** `PlayMode::stop` clears the world and reads
the snapshot back, and the snapshot is a scene document, so every entity comes back carrying
the identity the document holds. An entry on the stack finds its entity by asking for it by
name. Nothing had to be zipped, mapped or guessed, and the stack is never cleared at either
end.

The selection is kept the same way. It is an entity number, and every number changes at a
stop, so the editor reads the identity before the stop and looks it up after it. A play clears
nothing, so the selection simply stays.

**A scene restores its entities down two paths and each one puts an identity back separately.**
An entity the file lists goes through `take_identity` in `scene/scene_file.cpp`, and a prefab
instance goes through `assign_identities` in `scene/prefab.cpp`. A test that reaches only one
of them passes while the other is broken, which is why `tests/test_editor.cpp` edits one entity
of each kind. Deleting either call fails that test and nothing else it drives.

An entry that names an entity no stop can bring back reports on the error channel and changes
nothing. That happens when something destroyed the entity outside the history, so the snapshot
never held it. Undo still moves past the entry, because the entries under it are good and
retrying a broken one forever is worse than skipping it.

The order-based mapping considered before the identity is worth recording as rejected.
`walk_in_order` in `scene/scene_file.cpp` pins its order by entity number, and after a clear
EnTT hands numbers out from a free list rather than in load order, so pairing a walk before a
play with a walk after a stop can quietly pair the wrong entities. An undo that moves
something else is a worse failure than one that is refused.

**It buys more than undo.** A component can name an asset by GUID today and nothing can name
an entity, so a trigger cannot say which door it opens and a camera cannot say what it
follows. This is the prerequisite for that, and M10 and the game will both want it.

**Escape clears the selection rather than closing the window.**
`platform::WindowDesc::quit_on_escape` is a per-application choice, the way docking is for
`gfx::ImGuiDesc`. A game runtime quits on Escape and still does. The editor does not, because a
key that throws away unsaved work when somebody meant to deselect something is the worst kind
of shortcut. File > Exit is the way out. Escape is also the one editing key that still works
while a session runs, because letting go of a selection is not an edit.

**Pressing Delete takes the selected entity and asks nothing.** The question the World panel
asks was written when a delete was final, and a key that stops to ask is a key nobody uses.
The button keeps its question, because it also says how many entities go with the one that
was picked, and that is worth seeing before fifty of them do.

**Undo is off while a play session runs**, and so is redo. The world under a session is a
game part way through a step, and every entry on the stack belongs to the scene somebody
authored. Undoing into a running game would move an entity the simulation owns, and a stop
reads the snapshot back over it, so the change would not survive the session either. The save
button is already off for the same reason. `editor::undo_menu` holds that rule, and it takes
the names off the two items as well, because naming an entry nobody can click invites somebody
to try.

**What a step costs, measured.** The largest scene the project has is Intel Sponza at 180
entities, and the sandbox is 43. The numbers below are the serialized size of what an entry
keeps and the wall time of one undo, taken over 20000 entries and repeatable across runs.

| | sandbox, 43 entities | Sponza, 180 entities |
|---|---|---|
| One field edit keeps | 2 x 151 B | 2 x 155 B |
| One delete keeps | 531 B | 409 B |
| One undo step | 0.49 us | 0.50 us |
| A whole-world snapshot | 8344 B, 0.26 ms | 9408 B, 0.53 ms |

**An entry costs the same on both scenes and a snapshot does not.** That is the whole of the
transactional decision, and it is the shape rather than the extreme: 180 entities is not a
level worth building either, and the snapshot column is what grows when it becomes one. Sponza
is large in triangles rather than in entities, because its geometry sits in cooked assets and
its scene file is mostly prefab instances that collapse to one record each.

The in-memory cost of an entry is larger than the serialized figure, because a
`nlohmann::json` document carries its own nodes. Measuring it through resident pages was tried
and thrown away: the second scene measured reuses the pages the first one freed, so it reported
14 bytes for an entry that plainly costs more.

**Done when:** every edit can be undone and redone, an action is one entry, an edit still
names its entity after that entity has been deleted and brought back, and the cost of a step
is measured on the largest scene the project has rather than on the sandbox.

### M13 — Source assets in the editor
The editor opens a project that has never been cooked. It reads `.gltf`, `.png`, `.hdr` and
`.glsl` straight out of the source tree, imports them in memory, and draws them. **The
runtime stays cooked-only.**

The runtime keeps its side for two reasons. A shipped game should not carry cgltf, stb,
meshoptimizer, bc7enc and shaderc, and shaderc is the largest of them. And cooked content is
what makes a run reproducible, which is what every offscreen capture in this project rests
on.

**The rule that stops this becoming two engines: the editor runs the same import code the
cooker runs, and produces the same bytes.** An entity drawn in the editor is drawn from the
data the runtime will draw it from, so the two pictures match and a capture is still worth
comparing. Anything that departs from that is a deliberate exception, named here, with its
reason.

There is exactly one place to put the seam. Every cache and pass reads an asset through
`assets::Content`: `MeshCache`, `TextureCache`, `MaterialCache`, `MeshPass`,
`SceneRenderer`, `sandbox::load`, and `play::Session::load_scripts`. So an interface there,
with a cooked implementation and an importing one, reaches everything and changes nothing
above it.

**M13.1 put it in, and the callers ask three questions rather than two.** `assets::AssetSource`
answers what a source path names, what the project holds of a kind, and what the bytes for an
identity are. The third one was expected. The first has to answer with a list, because one file
holds several assets: a glTF holds a mesh for each primitive, and a shader with a variant list
holds a module for each form. **The order of that list is load-bearing**, because
`mesh_variant_index` indexes into it, so a source that answered out of order would bind the
wrong pipeline and draw a picture that looks right.

The second question was the one the shape did not predict. `Session::load_scripts` and
`sandbox::add_prefabs` hold no path to ask about, so both walked the whole manifest looking for
an extension. `assets_of_kind` is that walk, and it is what stops those two reaching for a
cooked tree.

An `AssetRecord` carries a name beside the identity, and the name is not decoration.
`prefab_name` reads it to work out the key a prefab goes into the library under, so an
importing source has to give an asset the name the cooker would for the same reason it has to
give it the bytes the cooker would.

**Five callers keep naming `Content`, for two different reasons.** Three of them mean a cooked
tree: `HotReload` cooks and compares manifests, the asset browser lists what a cook produced,
and `save_scene_source` turns identities back into source references through the manifest,
which is what M13.4 replaces. The other two, `ui::ImageFactory` and `ui::FontFactory`, do not
mean a cooked tree at all. They resolve a layout's absolute path against the content root, and
a root is not a question this interface answers.

**M13.2 moved the importers into `src/import/`, as `engine::import`.** `tools/cooker/` keeps
`main.cpp` and nothing else, so the cooker is the command line over those rules rather than
their owner. The library was separable before the move: `cooker_lib` already knew no game, and
`tools/cooker` was already configured before `apps/editor`, so the editor could have linked it
where it stood. The work was therefore where the code sits rather than what it does.

It sits in `src/` because a directory two applications depend on is engine code, and because
`tools/` is for programs a build machine runs. Two things follow from the move that did not
follow from the old place. The Doxygen gate reads `src/` only, so these headers are now checked
like any other public header. And the namespace is `engine::import` rather than `cooker`, which
is what every other directory under `src/` does.

**Every importer dependency stays PRIVATE**, so stb, cgltf, meshoptimizer, bc7enc and shaderc
reach the cooker and the editor and never a runtime build. `nm` over the three binaries is how
that is checked rather than argued.

**The proof that a move is only a move is the bytes.** The cooker built before the move and the
cooker built after it were each run over both content trees, and the 85 cooked files are
identical byte for byte. A move that changed an import would show up there and nowhere else,
because no test reads a cooked file and compares it against another cooker.

**M13.3 split into an index and an import.** Every question but "what are the bytes" can be
answered from the source tree and its sidecars with no importer running, so #345 is
`import::SourceAssets` answering `assets_for`, `assets_of_kind` and a reference resolve, and
#363 is the import behind `read`.

**The index agrees with the cooker by construction rather than by coincidence.** A second copy
of what a file cooks into would drift the first time a rule changed, and the drift would be
silent: the editor would name an asset differently from the runtime and nothing would report
it. So the shared answers moved into `import/rules.h`. `rule_for` says what counts as content,
`cooked_name` says what a whole file is called, and `part_record` names a numbered part and
derives its identity. The glTF rule, the material rule, the prefab rule and the index all call
`part_record`, and `gltf_parts` and `cook_gltf` both take their inline image set from
`gltf_inline_images`.

**The proof is a cook and an index of one tree, compared as sets of the whole record.**
Comparing counts would pass while every name was wrong. Five mutations each fail it: a
forgotten irradiance, a forgotten prefab, a shader that gives one form instead of its variants,
a resolve that reads the wrong parent, and a glTF buffer treated as an asset.

**A reference resolves against the sidecar of the named file, not against the first asset that
file produced.** A glTF names only derived parts, so no record of one carries the identity of
the file itself. Taking the first record resolved every reference to a glTF against its first
mesh. The test caught it, and it caught it only because it compared against the identity the
part actually goes by rather than against "not the other one".

**A `.bin` buffer proves nothing about the buffer skip.** Nothing gives `.bin` a rule, so the
tree walk already drops it and the skip is never consulted. The skip does work only when a
buffer file has an extension that carries a rule, so the test names its buffer `payload.lua`.
The cooker's own copy of that skip is still unreached by any test, which is #364.

**M13.3b routes every rule through a writer, and the editor gives it memory.** A rule used to
open its own destination file. There were nine such sites, and each already held the finished
bytes when it opened the stream. They write through `import::Writer` now: `FileWriter` for the
cooker, `MemoryWriter` for the editor. So an import needs no cooked tree and no place to put
one, and the bytes come from the same rule either way.

The proof is the same as M13.2's, plus one more. The cooker's output is byte for byte what it
was over both content trees. And every asset a cook produced, read back through the import and
compared against the file on disk, is identical. Four mutations fail that suite: a cache that
is never consulted, a read that answers another asset's bytes, an irradiance payload sized by
`size()` rather than `sizeof`, and the M13.3a set.

**What an import costs**, measured with `tools/cooker/measure.cpp` over the two content trees
on the reference machine. Each number is the first read of an asset, which is the read that
runs the rule.

| Source | Files | Each | Total |
|---|---|---|---|
| `.hdr` environment | 1 | 1797 ms | 1797 ms |
| `.brdf` table | 1 | 1692 ms | 1692 ms |
| `.png` texture | 17 | 37.8 ms | 642 ms |
| `.frag` shader | 4 | 136 ms | 543 ms |
| `.vert` shader | 5 | 79 ms | 396 ms |
| `.comp` shader | 1 | 84 ms | 84 ms |
| `.gltf` model | 5 | 4.0 ms | 20 ms |
| documents and scripts | 7 | under 0.3 ms | 0.7 ms |

**Two assets are the whole problem and neither is geometry.** The environment prefilter and the
split sum table are each about 1.7 seconds, and together they are three quarters of the 4.2
seconds both trees cost cold. Everything a person actually edits is cheap: a glTF is 4 ms and a
texture is 38 ms.

That is what makes on demand the right answer rather than a shortcut. A scene that names no
environment never pays the 1.8 seconds, and no asset is imported until something asks for it.
The BRDF table depends on nothing at all, which is why #366 proposes shipping it rather than
integrating it in every editor session.

**M13.4a made the editor's remaining cooked-tree assumptions into project questions.** Three
things named a cooked tree and meant "what the project holds". `SourceAssets` answers with a
`Manifest` now, built from the index it already has, so `assets::reference_for`,
`scene::restore_references` and the asset browser work over a source tree with no change of
their own. The panels take a `Manifest` rather than a `Content` for the same reason.

**`sandbox::load` reads its opening scene through the asset seam** rather than opening the file
itself. That is behaviour-preserving for a cooked tree and it is what makes a source tree work
at all: reading a `.scene` through the seam runs the document rule, and the document rule is
what resolves the references a source scene names by path.

**The two trees build the same world**, which is the claim the whole milestone rests on. The
test loads the shipped sandbox from the cooked tree and from the source tree and compares the
two worlds as documents. It compares them without the entity identities, because an entity that
comes back with no id in the file is given a fresh one and no two loads agree there. Everything
that says what the world is stays in.

**A document rule resolves a reference only inside a component it knows.** The first version of
that test built a registry by hand, missed `ScriptComponent`, and the source load failed on
`asset:scripts/spin.lua` while the cooked load was fine. The cooker never meets this because its
executable links the game. An editor that registers less than the cooker cannot open what the
cooker wrote.

**M13.4b points the editor at the source tree and the cook-on-save is gone.** `load_world`
opens `sandbox/content` through `import::SourceAssets`, so the tree the editor saves into is
the tree it reads. That is what #341 asked for: the workaround there was to run the cooker
after every save, because the editor wrote one tree and read another, and every edit looked
lost while the file on disk was right.

**A registry has to be set before an asset is imported, not after.** The document rule resolves
a reference only inside a component it knows, so `set_components` comes before `open`. Without
it a scene naming `asset:scripts/spin.lua` fails to import and the editor comes up with an
empty world and no obvious cause.

Verified by deleting the cooked game tree and starting the editor: it imports the models on
demand and loads all 43 entities of the sandbox from source.

**M13.5 reimports a source file that changed, and there is no cook in it.**
`platform::DirectoryWatcher` already polls a tree and holds a change back until the file stops
moving, so the editor polls it between frames and hands the changed paths to
`SourceAssets::reload`. A file that changed is a cache entry to drop, and the next read of it
imports again.

**The changed list comes from the watcher rather than from a diff of the index.** An index says
what a tree holds, not what is inside the files, so editing the pixels of a texture changes
nothing it can see. `assets::Content` can diff two manifests because the cooker hashes every
input. A source index has no hash and needs none.

The whole tree is indexed again on a change all the same, because a change can alter what a
project holds rather than only what is in it: a glTF gains a mesh, a shader gains a variant, a
file is deleted. So the reload reports what came and what went as well as what moved.

**A texture swaps under the entities that name it, and a scene or a prefab rebuilds the world.**
Both were run rather than argued: touching `crate.png` while the editor runs reloads one asset
and leaves the world standing, and touching `main.scene` rebuilds all 43 entities about two and
a half seconds later, which is the watcher settling.

**Nothing reloads while a play session runs.** The world under one is a game part way through a
step, and a stop reads a snapshot back over anything a reload did. That is the same reason undo
is off during play.

**The undo history survives a reload when the entities do, which #371 settled.** An edit names
its entity by identity, and a scene file carries identities only from version 4, which is what
the editor writes. A scene the editor has saved therefore gives the same entities back and the
whole stack is still good. The shipped sandbox scene is version 2 and carries none, so its
entities come back new and every entry would name an entity that is not there. Each one then
reports and does nothing, which is worse than an empty stack, so that case still clears.

**The question is asked of the world, not of the document.** #371 offered both: check that
every identity the stack names is in the world after the rebuild, or decide by whether the
document carried identities for every entity. The first is what `History::fits` does, because
it is the question the stack actually cares about. A document can carry identities and still
fail to give an entry its entity back, and then deciding by the document would keep a stack
that cannot run.

**Each edit answers for its own shape rather than for "the entity is present".** A create
expects its entity to be in the world and a delete expects it to be gone, so an entry whose
expectation the rebuild no longer meets is stale whichever way it disagrees. `Edit::fits` is
pure, so an edit added later cannot forget to answer.

**Import is not cheap and the answer is a cache, not a shortcut.** A cold cook of the sandbox
tree is 2.8 seconds for 30 assets, most of it BC7 and the environment prefilter; an
incremental one is about a tenth of a second. So the editor imports one asset at a time and
keeps what it imported for the session. If that is not enough, the next step is a cache of
imported bytes on disk, and that cache is a cooked tree with another name. It should be
called one rather than becoming a third format.

Cooking then becomes something a person asks for, because it is how a level reaches the
runtime rather than how the editor sees its own work.

**M13 is complete.** The editor opens the sandbox with no cooked game tree, imports what it
draws, and cooks for the runtime when a person asks. `File > Cook project` is that ask, and it
reports what it wrote by reading the manifest back rather than by trusting the exit code.

**The proof is the bytes, not the pixels, and that is a decision.** Every one of the 69 assets
the sandbox ships is imported by the editor byte for byte identical to what the cooker writes,
checked over the shipped tree rather than a tree a test built. The scene document resolves the
same both ways, which M13.4a checks by building the world twice and comparing it. The picture
is a function of those two things and the renderer, and both applications draw through one
`render::SceneRenderer`, so an identical picture follows.

**A pixel comparison is not the instrument here, and the reason is worth recording.** The
editor renders the scene into a panel image and composites ImGui over it, so an offscreen
capture of the editor would hold the panel furniture rather than the scene, and ImGui does
nothing offscreen at all. Comparing a windowed editor capture against an offscreen runtime one
is refused by the rule in `CLAUDE.md`: a windowed capture is whatever size the window manager
chose. Making the two comparable needs a render path in the editor that skips the panels and
draws straight to the device target, which is #377.

Issue #190 wanted a capture of the scene alone as well, and it got one first: `editor
--offscreen` already draws through the scene camera with no panels, and `tests/test_gpu_frame.cpp`
reads that capture. What #377 still wants is the comparison against a runtime frame, which needs
the two to agree on more than "neither is blank".

**Done when:** the editor opens the sandbox with no cooked **game** tree and draws it, a level
built that way runs in the runtime after one cook, and the two pictures match or every
difference is named here.

**The engine content tree stays cooked, and that is a decision rather than an omission.**
`src/render/content` holds ten shaders and the split sum table, and `SceneRenderer::create`
builds every pipeline at startup, so none of it can wait for something to ask. From source it
is about 2.7 seconds on every editor start: 1692 ms for the table and about 1023 ms for the
shaders. The build already cooks that tree as a build step, so it is always present and never
stale, and a project in this milestone means the game rather than the engine. Deleting the
cooked game tree is what the editor has to survive. Deleting the cooked engine tree is not.

M13.4b was measured before this was settled, and #366 is what would make the other answer
cheap.

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

**M9.8 is done, and M9 is complete.** It waited for M13, because M13 changed how the editor
gets its assets and running the test before that would have proved a loop that was about to
change underneath.

**Hard rule 3 holds, re-checked after M13 rather than before it.** The runtime built with
`ENGINE_WITH_EDITOR=ON` and with it off gives a byte-identical offscreen capture of the
sandbox. `WITH_EDITOR` removes code and does not change what the remaining code does.

**A two-build comparison has to hold every other option still.** The first attempt reported a
difference and it was the harness: a fresh build directory given only
`-DENGINE_WITH_EDITOR=OFF` also defaulted `ENGINE_WITH_UI` to off, so one runtime drew the game
UI and the other did not. CMake says nothing when it defaults an option you did not name.

**The editor and the runtime draw the level the same way**, and the two differences are named
rather than tolerated. The runtime draws the game UI over the scene and the editor draws the
scene alone. And the runtime steps physics and scripts, where an offscreen editor frame draws
the world as authored. With those accounted for the two agree on geometry, framing and shading.
`editor --offscreen --screenshot` is what makes that comparison possible at all, and it is
reproducible across a full rebuild.

**The authoring pass found nothing worth filing.** A level was built, saved, and cooked in the
editor, and the runtime ran it. That is a weaker result than M8.6's, which turned up four
engine gaps, and it is recorded because an exercise that found nothing is only evidence when
somebody says it was actually run. The editor is expected to gain and lose things as it is
used, so this is a working tool rather than a finished one.

**M13 runs after M12.** Both are editor comfort and both are worth having, and undo comes
first because every hour of authoring without it is an hour spent being careful. M13 also
wants an editor that is already pleasant to use, because it is a change to how assets reach
it rather than to what a person can do.

**M12 runs before M10.** Undo is authoring, and authoring pain compounds: every level built
without it is built carefully rather than freely, and every mistake costs whatever was not
saved. M9 gives a person the tools to build a level, and M12 is what makes using them
comfortable. The game UI can wait a milestone for that.

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

1. ~~Does Box3D use +Y up?~~ Answered in M7.1. It does, and it is right-handed and MKS as
   well. See §3.
2. What does the `moth_graphics` Vulkan backend already implement for font atlasing and text
   layout, and how much can you reuse? See §8.3.
3. Does `moth_editor` become a panel in the engine editor, or stay standalone? Not urgent.
   Revisit after M9. See §8.5.
4. Should moth_ui drop `fmt` and `range-v3` for `std::format` and `std::ranges`? This matters
   only if moth_ui moves to C++20. It is cosmetic, and it removes two transitive
   dependencies.
