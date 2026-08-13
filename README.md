# Camina Engine

A small 3D game engine in C++20. Vulkan rendering, EnTT, and Lua scripting.

The C++ namespace is `engine`, not `camina`. See [DESIGN.md](DESIGN.md) section 2.1
for why.

Read [DESIGN.md](DESIGN.md) for the architecture, the milestones, and the reasoning
behind each decision. Read [CLAUDE.md](CLAUDE.md) for the working rules.

## Status

M0 through M8 are complete. M9, the editor split, is next.

The runtime draws a scene of glTF models through Vulkan, shaded by Cook-Torrance
metallic-roughness and lit by an HDR environment through the split sum approximation. A
directional light casts a cascaded shadow, and a compute pass culls the point lights into a
cluster grid. The scene renders to a half float target and an ACES curve tonemaps it. A
render graph derives the barriers between passes from what each pass declares.

Assets are cooked by `tools/cooker/` and reload while the program runs. A scene holds an
EnTT world with a transform hierarchy and prefab instances.

Box3D runs on the engine job system at a fixed step, and a frame interpolates between the
last two steps. A stack of crates stands, and a crate thrown at it knocks it over.

**The sandbox game is Lua, all of it.** A script runs on the same fixed step, reads and writes
any reflected component by name, and reaches the world, prefabs, physics, input and the camera
through a small hand-written surface. A trigger volume reports what crossed it and a contact
reports what touched what, both on the step where it happened. Editing a script changes the
running game, and a text that will not compile leaves the old one running.

The game itself is a physics puzzle in two files. `spin.lua` turns what the scene gives a Spin,
and `puzzle.lua` owns the throw, the win and the reset. Neither `sandbox::update` nor a C++
throw exists any more, so the tests drive cooked scripts through the callbacks.

See DESIGN.md section 10 for the milestone list, and CLAUDE.md for the detail of what has
landed.

## Requirements

The engine targets two platforms. Each one has one compiler.

| Platform | Compiler | Conan profile |
|---|---|---|
| Linux | Clang 19 or later | `linux-clang` |
| Windows | MSVC, Visual Studio 2022 | `windows-msvc` |

CI builds and tests both on every pull request. GCC is not a target.

You also need:

- CMake 3.28 or later.
- Ninja.
- Conan 2.
- The Vulkan loader and headers, from M1 onward.

On Debian and Ubuntu, install the windowing packages that SDL3 needs:

```bash
sudo apt-get install -y ninja-build pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev \
    libxi-dev libxkbcommon-dev libwayland-dev wayland-protocols \
    libegl1-mesa-dev libgl1-mesa-dev libasound2-dev libpulse-dev libudev-dev
```

## Build

On Linux:

```bash
conan install . -pr:h profiles/linux-clang -pr:b profiles/linux-clang -b missing
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
```

On Windows, start a "x64 Native Tools Command Prompt for VS 2022" so that `cl.exe` and
Ninja are on PATH. Then run the same commands with the Windows profile:

```bat
conan install . -pr:h profiles/windows-msvc -pr:b profiles/windows-msvc -b missing
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
```

Both profiles ask for the Ninja generator, so the preset names match on each platform.

Use `linux-clang-asan` for a build with the address sanitizer and the undefined behavior
sanitizer. That profile turns Tracy off, because Tracy and the sanitizers both want the
signal handlers.

## Run

```bash
./build/RelWithDebInfo/bin/runtime
```

On Windows the binary is `build\RelWithDebInfo\bin\runtime.exe`.

Press Escape or close the window to quit. Pass `--frames N` to exit after N frames, which is
what a scripted run and a screenshot use.

CI does not run the runtime, and it is not meant to. It builds both platforms and runs the
tests. A CI runner has no GPU, so drawing there would mean a software rasterizer whose pixels
are not those of any real driver, which would buy a smoke test at the cost of a second set of
expectations to keep. Checking what a frame draws is done on a real GPU, offscreen.

## The large test scene

The sandbox scene is small on purpose, and it tests one thing at a time. A renderer needs
a scene it cannot hide in as well, and that scene is too large for git. It lives in a
release asset instead:

```bash
python3 scripts/fetch-scene.py sponza
./build/RelWithDebInfo/bin/runtime --content build/RelWithDebInfo/bin/content/sponza \
    --watch scenes/sponza
```

The fetch downloads the archive, checks it against a SHA-256, unpacks it into
`scenes/sponza`, and cooks it. `--content` takes the cooked tree and `--watch` takes the
source one, which is why the run command names both.

Nothing fetches it during a build and nothing fetches it in CI. A clean clone builds and
tests with no network. `scenes/sponza/README.md` says what the archive holds, what was
done to the source, and how the scene is licensed and cited.

The scene renders: 3.75 million triangles over 115 meshes and 28 materials, lit by a sun
with four cascades and 22 lamps. It is what M5 was measured against, and it is the scene to
reach for when a change needs somewhere it cannot hide.

## Render without a window

```bash
./build/RelWithDebInfo/bin/runtime --offscreen --resolution 1280x720 --frames 120 \
    --no-watch --screenshot out.png
```

No window opens, and the capture is exactly the size asked for. Two runs of the same command
produce **the same image, texel for texel**, because an offscreen run advances the world by a
fixed step for each frame rather than by the clock. That is what makes a screenshot comparison
worth anything: there is no noise floor to subtract.

There is no overlay offscreen, because the ImGui backend needs the window. A capture with no
panels over it is the more useful one to compare anyway, so an offscreen capture and a windowed
one are not comparable to each other. Pick one and stay on it.

`--resolution` is exact only offscreen. Windowed it is a request, and a window manager is free
to hand back another size. That is why a run that has to be reproducible needs `--offscreen`.

Everything else is the same code either way. The passes, the barriers, and the color format do
not change, because a check that rendered differently from the program would be checking
something that does not ship.

## Measure a change

A run reports its frame time when it stops. Vsync is on by default, so those numbers are the
refresh rate. Turn it off to measure:

```bash
./build/RelWithDebInfo/bin/runtime --frames 600 --no-watch --no-vsync
```

Compare two runs with the **median**. The mean moves with a single hitch, and the low is the
best case. On the reference machine the median repeats to about 4 percent over five runs, so
a change under that is noise. The p99 and the high are far noisier, and they are there to
show a hitch rather than to bury it in an average.

The report drops the first 60 frames. Those build the pipelines and fill the caches, and they
run several times longer than a settled frame.

A period is wall time between the start of one drawn frame and the start of the next. It is
not GPU time, and it names no pass. See issue #133.

## Test

```bash
ctest --preset conan-relwithdebinfo --output-on-failure
```

This runs the unit tests and the rule 4.1 check, which proves that no file outside
`src/gfx/vulkan/` includes a Vulkan header. The rule 4.1 check is a shell script, so a
Windows machine without Git Bash does not get it. The `containment` job in CI runs on
Linux and covers every push, and it checks the Box3D headers the same way.

## Profile

Start the Tracy server, then start the runtime. The runtime connects on its own. Every
frame reports a `frame` zone, and each `parallel_for` reports one zone per partition.

## Format and lint

```bash
find src apps tests -name '*.cpp' -o -name '*.h' | grep -v '/version\.h$' | xargs clang-format -i
clang-tidy -p build/RelWithDebInfo $(find src apps tests -name '*.cpp')
```

CI fails on any formatting difference. clang-tidy also runs inside the compile step
for a Debug build, or whenever the `CI` environment variable is set.

An MSVC build skips clang-tidy, because clang-tidy reads a clang command line and the
MSVC driver passes flags it does not understand. The Linux job checks every file, so
nothing escapes review.

## Documentation

```bash
cmake -S . -B build-docs -DCAMINA_DOCS_ONLY=ON
cmake --build build-docs --target docs
```

This needs no Conan install. Open `build-docs/docs/html/index.html`.

Every public entity in a header carries a Doxygen comment. The docs build treats an
undocumented entity or a mismatched `@param` as an error, and CI runs it on every
push.

## Development flow

Development work happens on a branch and lands through a pull request. A small
documentation fix, a typo, or a one-line tweak can go straight to `main`.

`main` carries no branch protection rules. This flow is a convention, and an exception is
fine when the change is small.

1. Branch from `main`. Name the branch `feat/...`, `fix/...`, or `docs/...`.
2. Commit in the conventional style.
3. Open a pull request. CI runs the format check, the docs build, the containment check,
   and four builds: Linux with Clang and Windows with MSVC, each with the game UI off and
   on.
4. Squash merge, with a conventional-commit title. Each pull request then becomes one
   changelog entry.

## Releases

`version.txt` holds the version. Change it and push to `main`. CI then tags the commit,
generates `CHANGELOG.md` with [git-cliff](https://github.com/orhun/git-cliff), and
publishes a GitHub release. Each release carries one archive for each platform:

- `camina-<version>-linux-Release.tar.gz`
- `camina-<version>-windows-Release.zip`

Write commit messages in the conventional style, for example `feat:`, `fix:`, and
`refactor:`. `cliff.toml` groups them in the changelog. Commits without a prefix still
appear, under "Changes".

## Options

Pass these to `conan install` as `-o with_editor=True` and so on.

| Option | Default | Milestone |
|---|---|---|
| `with_editor` | False | M9 |
| `with_ui` | False | M10, and the M6 spike |
| `with_lua` | True | M8 |
| `with_audio` | False | M11 |

`with_ui` needs moth_ui in the local Conan cache. It is not on Conan Center.
