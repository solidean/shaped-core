---
created: 2026-11-09
refs:
  - test: "sg - an SGL buffer's length and a bytes' length are what the bound memory holds (CHK-460)"
---

# Buffer lengths reach Metal shaders (EMIT-195)

A Metal buffer carries no length, and sg passes none in, so SGL's `.length` and every guard measured against a length have nothing to read on metal (EMIT-195).
Skipped on metal until then: sg's buffer-length test, and sr's three debug-output tests that guard a binding array's element.
Hypothesis, not tried: the argument buffer holds each buffer's byte length beside its address, as the hit-offsets mirror rides in its tail.
