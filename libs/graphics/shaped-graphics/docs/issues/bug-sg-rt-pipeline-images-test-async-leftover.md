---
created: 2026-10-20
refs:
  - test: "sg - an SGL ray-tracing pipeline's raygen and miss store into images"
  - file: libs/graphics/shaped-graphics/tests/pipeline/raytracing-pipeline-test.cc
---

# The SGL ray-tracing pipeline's image test once left async work running, single-threaded on vulkan

In a 2026-10-20 `dev.py check` run, the `singlethreaded-clang` preset's shaped-graphics-test reported the test's vulkan instance as having left one async item carrying its context.
The same binary ran it 20 times in a row on its own afterwards, with `--repeat 20`, and passed every time, so it has been seen once, under the whole binary's load.
Nothing about the shaders it compiles had changed in a way that touches async work; the commits under test were SGL front-end and legalizer changes.
