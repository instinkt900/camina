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

M4.5 closes the milestone. `platform::DirectoryWatcher` polls the source tree and holds a
change back until the file stops moving. `platform::run_process` starts the cooker without a
shell. `assets::HotReload` joins them, and `Content::reload` compares the new manifest against
the old one so a save names one asset rather than the whole tree. `MeshPass::reload` waits for
the frames in flight before it frees anything. The runtime watches `sandbox/content`, and a
scene or a prefab that changes builds the world again. `--watch`, `--glslc`, and `--no-watch`
override it.

Verified on 2026-08-04 with Clang 19, CMake 3.28.3, and Conan 2.31.1, on an NVIDIA
GeForce MX250 with the Khronos validation layer active. A texture and a scene reloaded
together in a running program, with no validation message. The build produces no warnings
under the full warning set.

## Development flow

**Do all development work on a branch and merge it with a pull request.** Commit
straight to `main` only for a small documentation fix, a typo, a comment change, or a
one-line tweak. When you are not sure which one applies, use a branch.

`main` carries no branch protection rules, and the user does not want any. This flow is
a convention, not a lock. The user can set it aside at any time. Follow an explicit
instruction to work on `main`.

Steps:

1. Branch from current `main`. Name the branch `<type>/<short-topic>`, for example
   `feat/vulkan-swapchain` or `fix/arena-alignment`.
2. Commit on the branch in the conventional style.
3. Open a pull request with `gh pr create`. Write the title in the conventional style.
   A squash merge uses that title as the commit subject.
4. Wait for CI. The format, docs, vulkan-containment, and build jobs all run on a pull
   request.
5. Read the automated code review as well. It runs on each push, and it often finishes
   after the other jobs. A green CI therefore does not mean the pull request is clear.
6. **Collect every change before you push again.** Gather the CI failures, the review
   comments, and any work you still owe the branch. Fix them together, and push once.
7. Ask the user before you merge. Do not merge your own pull request on your own.

Step 6 matters because a push restarts every CI job and starts a new review. Two pushes
five minutes apart cost two full runs and two reviews, and the second review reads a
branch the first one already covered. Waiting costs nothing, because the review has to
arrive before the branch is ready either way.

Squash merge is the default here. `cliff.toml` skips merge commits and strips the
`(#12)` suffix that GitHub adds to a squashed subject, so one pull request becomes one
changelog entry. A merge commit works too, but then every work-in-progress commit on the
branch reaches the changelog.

**No co-author trailers.** Do not add a `Co-Authored-By:` line to any commit message.
This overrides the default Claude Code behavior. The author of a commit is the person who
owns the repository.

Change `version.txt` in its own small commit on `main`, or in a release pull request of
its own. A push to `main` that changes `version.txt` starts a release.

## Issue tracker

`DESIGN.md` §10 defines the milestones. The GitHub tracker holds the state.

- **One GitHub Milestone for each `DESIGN.md` milestone**, M0 through M10, plus M5.5.
- **Issues are work increments, not milestones.** M1 was one line in `DESIGN.md` and became
  three pull requests. Split a milestone the same way, and name the issues `M<n>.<k> — ...`.
- **An issue links to its `DESIGN.md` section. It never copies the definition.** Two copies
  drift. The issue body holds the task list and the state.
- **Create issues for the milestone in progress and the next one.** A detailed ticket for
  M8 written today will be wrong by the time it starts.
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
- **`.clang-tidy` trap.** Never put a comment inside the `Checks:` block. It is a YAML
  folded scalar, so a comment line without a trailing comma merges with the entry
  after it, and that entry stops working with no error. Put rationale above the key.
- **Third-party headers and clang-tidy.** `HeaderFilterRegex` is `.*`, so clang-tidy
  reads every header our code includes, including the ones in the Conan cache.
  `ExcludeHeaderFilterRegex` drops `~/.conan2/`. `SKIP_LINTING` on a source file does
  not help here, because a third-party header arrives through our own translation unit.
  The ImGui Vulkan backend header is the case that needed this.
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
- **Submodules.** `third_party/bc7enc_rdo` is the first one. A fresh clone needs
  `git submodule update --init --recursive`, and `third_party/bc7enc/CMakeLists.txt` fails
  the build with that command in the message when the directory is empty. Only
  `bc7enc.cpp` is compiled. The rest of that repository holds an ISPC kernel, a PNG
  reader, and a DDS writer that we do not use.
- **Third-party sources we compile.** `SKIP_LINTING` is a source file property, not a
  target property. Setting it on a target does nothing and reports nothing.
  `gfx/vulkan/vk_vma.cpp`, `tools/cooker/stb_image_impl.cpp`, and `bc7enc.cpp` all carry
  it on the file, with `-w` alongside.
- **EnTT assertions.** `src/core/entt.h` points `ENTT_ASSERT` at `ENGINE_ASSERT`.
  Include it before any EnTT header. Every engine header that includes one does that
  already, and the file fails the build with a message when the order is wrong.
  Without it EnTT falls back to `assert()`, which `NDEBUG` removes from a
  RelWithDebInfo build, so a `get<T>()` for a component that is not there kills the
  process with no message.
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

ImGui itself is not behind `with_editor`. Hard rule 3 says the editor is an application
and not a build mode, and the M2 inspector runs as a debug overlay in the runtime. The
option still gates ImGuizmo and, from M8, the editor application.

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
| Game UI | moth_ui. ImGui is for the editor and debug overlays only |
| Networking | Not being built. Keep the three enabling decisions in `DESIGN.md` §9 |

## Sequencing

- Build M4, the asset pipeline, before M5, the renderer. A renderer with no asset pipeline
  draws only programmer cubes.
- Start `sandbox/` at M3 and keep it working.
- Keep the M5.5 moth_ui spike timeboxed. Its value is interface feedback, not pixels.
