---
created: 2026-11-09
refs:
  - test: "sg - an SGL buffer's length and a bytes' length are what the bound memory holds (CHK-460)"
closed: 2026-11-09
resolution: fixed
---

# Buffer lengths reach Metal shaders (EMIT-195)

A Metal buffer carries no length, and sg passes none in, so SGL's `.length` and every guard measured against a length have nothing to read on metal (EMIT-195).
Skipped on metal until then: sg's buffer-length test, and sr's three debug-output tests that guard a binding array's element.
Hypothesis, not tried: the argument buffer holds each buffer's byte length beside its address, as the hit-offsets mirror rides in its tail.

## Resolution

Each group's argument buffer now holds one `u32` byte length per slot behind its slots, and SGL's MSL group struct declares it as `uint sgl_lengths[n]`.
sg writes them when it mints a group, direct and staged, from each buffer view: `element_count * stride` for a structured view, `size_in_bytes` otherwise.
`.length` reads the slot's entry divided by the element's size, and a `bytes`' as is, so metal compiles guard buffer indices like every other target.
sg's buffer-length test and sr's three binding-array guard tests run on metal, and the hit-offsets mirror sits behind the lengths.
