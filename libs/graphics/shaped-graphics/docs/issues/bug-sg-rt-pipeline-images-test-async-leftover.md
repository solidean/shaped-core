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

Seen again in `dev.py check`'s `singlethreaded-clang` leg on 2026-10-21, after 9855c3126, which touched only SGL's target text; the binary passed when run again.

Seen again on 2026-10-30 in the same leg, with the machine under heavy load from other work; `--repeat 10` alone passed every time.
That is three sightings, each in a full run and none alone, which points at timing under load rather than at the change under test.
A fourth time the same day, in the next full `check`, with the load unchanged: under sustained load it now shows in nearly every full run.
