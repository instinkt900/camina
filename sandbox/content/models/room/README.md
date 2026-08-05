# Room

`room.gltf` is the interior everything else stands in. Five walls, generated rather than
modelled: a floor, a ceiling, a back wall, a red wall on the left, and a green wall on the
right. The front is left open, so the camera starts inside and can still fly out.

## Why the sandbox needs it

Open space cannot test a shadow, because nothing occludes anything. It cannot justify several
lights in a small volume either. Walls and a ceiling give both, which is what M5.5 and the
light work need. See issue #126.

The coloured walls are not decoration. A metal sphere near the red wall reflects red, and the
row of spheres picks up both walls across its length. That is the cheapest visible test that
the prefiltered environment is being sampled along the reflection and not along the normal.

## Why it is generated and not a real scene

A real interior was measured and rejected twice.

- **Khronos Sponza** is `LicenseRef-CRYENGINE-Agreement`, owned by Crytek, covering every file.
  That is a proprietary licence and not the CC-BY it is often assumed to be.
- **Intel Sponza** is CC BY 4.0 and a good scene, and its geometry is a 133 MiB `.bin`. GitHub
  refuses any file over 100 MiB, and the triangles are spread evenly enough that dropping
  detail meshes does not help.

Issue #130 holds the large scene, fetched from outside git rather than committed. This room
stands in until then, and it will still be useful afterwards because it is small enough to
reason about.

## The colours are darker than they look

The base colours are the usual Cornell values scaled to about a third. Nothing in the renderer
controls exposure yet, and there is no tonemap until M5.6, so a bright environment lights a
white floor past 1.0 and it clips to flat white.

That is measurable rather than a matter of taste. At the full Cornell values, 40 percent of the
viewport clipped with **every light switched off**, because the image based ambient alone was
enough. At a third, it is under 1 percent, and what remains is specular highlights on the metal
spheres, which are meant to be bright.

Put the values back when #88 lands an ACES tonemap. The scale is one number in the generator.

## How it was made

A short Python script writes the file: five quads, one material each, in a single mesh. Each
face declares the normal it wants, and the script reverses the corner order when the cross
product disagrees. So a wall cannot end up inside out through a typo in a corner list, which is
the failure that is hardest to see in a closed room.

The buffer rides in a data URI, so the model is one text file. `crate.gltf` explains that
choice.

The `.meta` sidecar is in version control. The identity in it is what the manifest resolves the
prefab from, so deleting it would break the reference `main.scene` holds.
