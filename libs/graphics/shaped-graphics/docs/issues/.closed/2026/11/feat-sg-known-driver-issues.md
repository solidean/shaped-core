---
created: 2026-11-25
tags: [testing, drivers, bugs-external]
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/adapter_info.hh
  - file: docs/bugs-external/_index.md
  - file: libs/graphics/shaped-viewer/tests/raster-test.cc
  - file: libs/graphics/shaped-rendering/tests/debug-output-test.cc
  - file: libs/graphics/shaped-graphics/tests/pipeline/pixel-semantics-test.cc
  - file: libs/graphics/shaped-graphics/tests/pipeline/raytracing-pipeline-test.cc
closed: 2026-11-25
resolution: fixed
---

# Known driver bugs as one registry that tests skip by, and that a run can still try

Every test a driver bug from `docs/bugs-external/` trips over writes its own check today: a vendor id, a backend, sometimes a hand-parsed driver version, and a `SKIP` whose message names the entry.
Nothing lists them, the checks drift apart, and a skip never notices that the vendor fixed the bug.

Wanted:
- **One registry in sg**: each entry is a bugs-external id, its symptom in a line, and which contexts it affects.
  A test asks it by id instead of spelling the condition out.
- **A run that tries them anyway**: `dev.py test --known-issues` runs what the registry would skip, so a pass says the entry can retire.
- **`sg::adapter_info` names the driver**: `driver_version` is opaque, so RADV, AMD's own Linux driver and AMD's Windows vulkan driver look alike.
  Vulkan reports `driverName` and `driverInfo` (`"radv"`, `"Mesa 26.2.0-devel"`).

The first users are the five checks there are: Intel's view instancing and hit-object pipeline, NVIDIA's mesh atomics, Intel's cached PSO in shaped-viewer, and RADV's BC7 encoder.
