---
created: 2026-11-12
refs:
  - file: libs/graphics/shaped-viewer/tests/emitter-trace-test.cc
  - test: "sv - a per-triangle lamp of two different triangles lights as two constant ones do"
  - test: "sv - the Cornell box's per-triangle lamp lights the box as a constant one does"
---

# The per-triangle lamp tests fail on the wavefront under another random sequence

Measured, 2026-11-12: with one more value folded into the view's trace hash, which is the key its random sequence is drawn from, both tests failed on the wavefront, every run.
- Two different triangles: block (0, 0) channel 0 held 0.467 against 0.229, an error of 0.61 against a tolerance of 0.08; channel 1 0.092 against 0.069.
- The Cornell box: block (0, 0) held 0.142 against 0.095, an error of 0.22.
- The megakernel passed under the same change, and both tests pass under the hash as it is.

The change was taken out of the hash, since it did not belong there; nothing else differed between the passing and the failing runs.
So either the tests sit near their tolerance and one sequence crosses it, or a real bias of the wavefront's per-triangle emitter sampling shows under some sequences.
An error of twice the expected value in one block looks like a firefly, which would point at the light sample's pdf for one triangle.

To tell: run both against several seeds, and look at the block's per-sample contributions under a failing one.
