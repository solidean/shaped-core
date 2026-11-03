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
- **Seen again** on 2026-10-31, once more in a full `check` on `debug-nopch-clang`, with the second review's SGL commits in the tree.
- **Seen on `release-clang` too**, on 2026-11-02, once in a full `ship` gate; 5 of 5 isolated repeats passed.
- **Not measured**: whether vulkan sees it.
- **Unrelated to SGL's array and transpose commits of 2026-10-30**, which were in the tree: no sv shader holds an array in a block or calls `transpose`.
- **Hypothesis**, unverified: the restart test compares something that is not stable across frames for a megakernel layer, such as a value read back asynchronously.

## Found 2026-11-03, addressed in the test

- **Measured**: the warm-up traced only the default mode, and the megakernel compiles permutations of its own.
  So its first counted trace bound a stand-in in 3 of 6 full `shaped-viewer-test` runs on `debug-nopch-clang`.
  Those runs passed: `trace` absorbs a restart that leaves the count where it was, so a failure needs the restart to land after the count reached 2.
- **Not reproduced**: a failure with the stand-in in view; its link to the 1-after-3 is a hypothesis.
- The warm-up now traces a megakernel view too, until two frames accumulate with no stand-in bound.
  It stays open until full runs under load have gone a while without a repeat.
