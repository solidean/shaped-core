---
created: 2026-11-25
refs:
  - issue: feat-sgl-interpreter-bounds-float-departures
  - file: libs/graphics/shaped-graphics/tests/differential/differential.cc
---

# A bounding run widens a builtin by its precision on the device, measured, and widened further only with recorded evidence

EVAL-6 says a builtin is as precise as its target states, and a bounding run takes every builtin as exact.
So `sin`, `exp`, `rsqrt`, `pow` and a division through a reciprocal are covered only by a file's tolerance, which is a guess per file.

## Two bounds per builtin

- **The stated bound is the ceiling**: the precision Vulkan's, D3D's, WGSL's and Metal's specifications promise, per builtin and per argument range.
  A GPU outside it is wrong, and the test fails as a bug.
- **The measured bound is what the test uses**, per vendor and backend: what a measurement test found over a sweep of each builtin's inputs, committed as a table.
  It is tighter than the stated one, since the stated one is often loose to the point of saying nothing, `sin` to within 2^-11 absolute on Vulkan.
- A GPU between the two is no bug, but it is something the table did not know.
  The test then fails with the evidence, one record per element: GPU, driver, backend, builtin, input and output.
  Widening the table is an edit to a committed file, made with that record in front of you, never a tolerance bumped until the test is quiet.

## The shape

- A measurement test, run on demand like the references' refresh, writes the table.
  It sweeps each builtin over its domain, including its edges: around 0, around multiples of pi for `sin`, near `1` for `log`, and the denormals.
- `bound_call` widens a builtin's result by the table's bound for the context it is compared against, so the interpreter's ranges depend on the device.
  That is new: today one interpreter run serves every configuration.
- An unknown device takes the stated bound.

## Open

- Whether the measured bound should be a ulp count, an absolute error, or one per argument range, since `sin` near pi is absolute and `exp` is relative.
- Where the table lives: beside the references, since both are measurements of machines.
