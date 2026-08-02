# Camina Engine

A small 3D game engine in C++20. Vulkan rendering, EnTT, and Lua scripting.

The C++ namespace is `engine`, not `camina`. See [DESIGN.md](DESIGN.md) section 2.1
for why.

Read [DESIGN.md](DESIGN.md) for the architecture, the milestones, and the reasoning
behind each decision. Read [CLAUDE.md](CLAUDE.md) for the working rules.

## Status

M0, foundations. The engine opens a window, runs the job system, and reports to the
profiler. There is no renderer yet. See DESIGN.md section 10 for the milestone list.

## Requirements

- A C++20 compiler. GCC 13 or Clang 19 and later.
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

```bash
conan install . -pr:h profiles/linux-clang -pr:b profiles/linux-clang -b missing
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
```

Swap `linux-clang` for `linux-gcc` to use GCC. Use `linux-clang-asan` for a build with
the address sanitizer and the undefined behavior sanitizer. That profile turns Tracy off,
because Tracy and the sanitizers both want the signal handlers.

## Run

```bash
./build/RelWithDebInfo/apps/runtime/runtime
```

Press Escape or close the window to quit. Pass `--frames N` to exit after N frames, which
is what CI uses.

## Test

```bash
ctest --preset conan-relwithdebinfo --output-on-failure
```

This runs the unit tests and the rule 4.1 check, which proves that no file outside
`src/render/vulkan/` includes a Vulkan header.

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
3. Open a pull request. CI runs the format check, the docs build, the Vulkan containment
   check, and a build across GCC and Clang.
4. Squash merge, with a conventional-commit title. Each pull request then becomes one
   changelog entry.

## Releases

`version.txt` holds the version. Change it and push to `main`. CI then tags the commit,
generates `CHANGELOG.md` with [git-cliff](https://github.com/orhun/git-cliff), and
publishes a GitHub release with the runtime archive attached.

Write commit messages in the conventional style, for example `feat:`, `fix:`, and
`refactor:`. `cliff.toml` groups them in the changelog. Commits without a prefix still
appear, under "Changes".

## Options

Pass these to `conan install` as `-o with_editor=True` and so on.

| Option | Default | Milestone |
|---|---|---|
| `with_editor` | False | M8 |
| `with_ui` | False | M9, and the M5.5 spike |
| `with_lua` | False | M7 |
| `with_audio` | False | M10 |

`with_ui` needs moth_ui in the local Conan cache. It is not on Conan Center.
