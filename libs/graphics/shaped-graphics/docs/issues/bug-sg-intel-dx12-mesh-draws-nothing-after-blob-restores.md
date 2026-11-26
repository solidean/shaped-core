---
created: 2026-11-26
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_bind.cc
  - test: "sg - a mesh-shaded pipeline of triangles built from its own cached blob takes it, and draws as before"
---

# On the Arc, a mesh pipeline draws nothing now and then once mesh-shaded blobs were restored

On the Arc B580 (dx12 driver 32.0.101.9150), mesh pipelines drew nothing now and then once mesh-shaded pipelines had been restored from cached blobs.
Measured while fixing bug-sg-dx12-mesh-pipeline-blob-refused, on `uv run dev.py test "mesh"`:
under some seeds (7555890705584158751, 2820548425156727883), a pipeline built without a blob drew nothing, and so did its twin built from its blob.
The same seeds draw every pipeline with mesh blobs skipped, whether the stream states its topology or not, and AMD draws all of them with blobs restored.
So `dx12_bind.cc` restores no mesh-shaded blob on an Intel GPU.
Not known: what the restore leaves behind in the driver, and whether a standalone repro can show it.
