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

M0 is complete. The runtime opens a window, runs the job system across 8 workers,
resets a frame arena each tick, and reports to Tracy. There is no renderer yet.

M1 is next: Vulkan bring-up. See `DESIGN.md` §10.

Verified on 2026-08-02 with Clang 19, CMake 3.28.3, Conan 2.31.1, and Mesa 25.2.8 on
an Intel GPU. The build produces no warnings under the full warning set.

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
5. Ask the user before you merge. Do not merge your own pull request on your own.

Squash merge is the default here. `cliff.toml` skips merge commits and strips the
`(#12)` suffix that GitHub adds to a squashed subject, so one pull request becomes one
changelog entry. A merge commit works too, but then every work-in-progress commit on the
branch reaches the changelog.

**No co-author trailers.** Do not add a `Co-Authored-By:` line to any commit message.
This overrides the default Claude Code behavior. The author of a commit is the person who
owns the repository.

Change `version.txt` in its own small commit on `main`, or in a release pull request of
its own. A push to `main` that changes `version.txt` starts a release.

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
- **Shaders.** `cmake/Shaders.cmake` compiles GLSL to SPIR-V with glslc at build time
  and writes a braced list of 32-bit words. The consumer embeds it with
  `std::to_array`, so the runtime carries no shader compiler. glslc arrives through
  the `shaderc` tool requirement in `conanfile.py`. Conan Center has no binary for
  our profile, so the first install builds shaderc, glslang, and SPIRV-Tools from
  source. That takes several minutes once, then the cache serves it. M4 moves shader
  cooking into `tools/cooker/`.

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
3. **The editor is an application, not a build mode.** Use `#ifdef EDITOR` only to remove
   editor-only reflection metadata. Never use it to change engine logic.
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

```bash
conan install . -pr:h profiles/linux-clang -pr:b profiles/linux-clang -b missing
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
ctest --preset conan-relwithdebinfo --output-on-failure
./build/RelWithDebInfo/apps/runtime/runtime --frames 300
```

Windows uses the same commands with `profiles/windows-msvc`, from a "x64 Native Tools
Command Prompt for VS 2022". Both profiles ask for the Ninja generator, so the preset
names match on each platform. Two things differ on Windows. An MSVC build skips
clang-tidy, because clang-tidy reads a clang command line. The rule 4.1 test needs bash,
so it does not run without Git Bash. The Linux CI jobs cover both.

Conan lives in a virtual environment at `~/.venv/conan`. Add `~/.venv/conan/bin` to
PATH, or call the binary by its full path.

Conan options: `with_editor`, `with_ui`, `with_lua`, `with_audio`. Every one defaults
to False and turns on at its milestone. `with_ui` needs moth_ui in the local Conan
cache, because it is not on Conan Center.

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
