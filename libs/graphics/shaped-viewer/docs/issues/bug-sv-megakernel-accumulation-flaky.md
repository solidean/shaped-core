---
created: 2026-10-30
refs:
  - test: sv - a view accumulates across frames under its id
  - file: libs/graphics/shaped-viewer/tests/view-accumulation-test.cc
---

# A megakernel view sometimes counts 1 accumulated frame after 3 traces

`sv - a view accumulates across frames under its id`, section "a megakernel layer holds its frames' samples", fails now and then on dx12 hardware.
Three traces of an unchanged view leave `accumulated(v.id) == 1`, so something between them read as a change and restarted the accumulation.
The failing `REQUIRE` then makes the later sections unreachable, which is the rest of the report.

- **Measured**: once in a full `check` on `debug-nopch-clang`, and once in 12 with `uv run dev.py test "a view accumulates across frames under its id" --preset debug-nopch-clang --repeat 20`.
- **Not measured**: whether the other presets see it, or vulkan.
- **Unrelated to SGL's array and transpose commits of 2026-10-30**, which were in the tree: no sv shader holds an array in a block or calls `transpose`.
- **Hypothesis**, unverified: the restart test compares something that is not stable across frames for a megakernel layer, such as a value read back asynchronously.
