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

M5 is in progress, and the paragraphs below record it in the order the parts landed rather than
by number. The first half of M5.1 landed: a descriptor set layout now comes from the
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

A cell holds 256 light indices. That number is measured: at 64 the 837-light scene lost light on
36 percent of the frame, by up to 228 of 255 on a channel, and at 256 it matches a shader that
loops over every light byte for byte. So the grid was exact and the cap was the whole error. The
drop is still silent past 256, which is issue #175.

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
full screen pass that wrote it out, applying no curve, so the picture did not move. M5.6b put
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

M5 is down to three issues. #88 carries the done-when test and needs a Sponza-class scene, which
#130 has to fetch from outside git because the geometry is larger than GitHub accepts. #122 is
the aliasing half of the render graph, and its trigger condition is not met yet: the shadow map
and the scene color are both live, but the mesh pass reads one and writes the other in the same
pass, so neither can take the other's memory. #175 is the silent per-cell light drop.

Nothing culls a mesh against the frustum yet. `MeshPass::draw` walks every entity that names one,
whatever the camera is pointing at, and only the lights are culled. #130 names that as the reason
a Sponza-class scene would not be interactive, so it comes before #88 rather than after it.

Verified on 2026-08-09 with Clang 19, CMake 3.28.3, and Conan 2.31.1, on an NVIDIA
GeForce MX250 with the Khronos validation layer active. A texture and a scene reloaded
together in a running program, with no validation message. Two blended panes drew over the
opaque scene with no validation message either. Synchronization validation reports nothing over
300 frames offscreen and 200 windowed. The build produces no warnings under the full
warning set.

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

**`gh pr edit` cannot change a body here.** It asks for `projectCards`, which GitHub has
retired, and the call fails with a deprecation notice that reads like a warning. The edit
does not happen. Patch the body through REST instead, and read it back to check:

```bash
python3 -c "
import json, subprocess
body = open('body.md').read()
subprocess.run(['gh','api','-X','PATCH','repos/<owner>/<repo>/pulls/<N>','--input','-'],
               input=json.dumps({'body': body}), text=True)
"
gh pr view <N> --json body --jq '.body[0:200]'
```

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
  Require the expected count, which is **five**: `format`, `docs`, `vulkan-containment`, and
  the two `build` jobs.

Make the loop print the per-check result it decided on, so a wrong exit is visible in the
event rather than hidden behind the word "success". After the monitor reports, **query the
state once by hand** and report from that, not from the monitor's summary.

### Reading the review

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

**A rate limit ends the wait.** CodeRabbit posts its own limit as a top-level comment, with
a reset time. When it does, stop the monitor, say the review did not run, and hand back.
Do not wait for the reset, and do not poll for it. Whether an automated review is worth
waiting for is the user's call, not yours.

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

ImGui itself is not behind `with_editor`. Hard rule 3 says the editor is an application
and not a build mode, and the M2 inspector runs as a debug overlay in the runtime. The
option still gates ImGuizmo and, from M9, the editor application.

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
