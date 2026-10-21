---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_frame.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
---

# Tangent frames are quaternions, and not yet quantized

A mesh supplies `tangent_frame` — a unit quaternion taking tangent space to object space — plus `tangent_handedness`.
The hit builds its shading frame from that instead of the flat face normal.
The storage is an uncompressed `f32x4`.

The intended encoding is `quat10x3+i2` (see [zeux.io on quantizing tangent frames](https://zeux.io/2026/04/30/quantizing-tangent-frames/)).
That is a format change on one attribute plus a decode in the generated prologue — the same seam `attribute_interpolation` already opened.
It is not a content migration, because handedness deliberately lives beside the quaternion rather than in the sign of its `w`.

`quat10x3+i2` rather than the article's own `oct11x2+d9` pick, because a closest hit decodes three corners per hit and then blends them.
The quaternion is the form the blend wants, where an octahedral normal plus a diamond angle would have to be built into a basis per corner first.
