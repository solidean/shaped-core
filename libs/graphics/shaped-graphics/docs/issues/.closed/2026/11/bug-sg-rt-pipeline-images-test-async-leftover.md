---
created: 2026-10-20
refs:
  - test: "sg - an SGL ray-tracing pipeline's raygen and miss store into images"
  - file: libs/graphics/shaped-graphics/tests/pipeline/raytracing-pipeline-test.cc
closed: 2026-11-01
resolution: fixed
---

# The SGL ray-tracing pipeline's image test once left async work running, single-threaded on vulkan

In a 2026-10-20 `dev.py check` run, the `singlethreaded-clang` preset's shaped-graphics-test reported the test's vulkan instance as having left one async item carrying its context.
The same binary ran it 20 times in a row on its own afterwards, with `--repeat 20`, and passed every time, so it has been seen once, under the whole binary's load.
Nothing about the shaders it compiles had changed in a way that touches async work; the commits under test were SGL front-end and legalizer changes.

Seen again in `dev.py check`'s `singlethreaded-clang` leg on 2026-10-21, after 9855c3126, which touched only SGL's target text; the binary passed when run again.

Seen again on 2026-10-30 in the same leg, with the machine under heavy load from other work; `--repeat 10` alone passed every time.
That is three sightings, each in a full run and none alone, which points at timing under load rather than at the change under test.
A fourth time the same day, in the next full `check`, with the load unchanged: under sustained load it now shows in nearly every full run.

Seen again on 2026-10-31 in shelley's `ship` gate (`singlethreaded-clang`), under a commit that touched only babel's glTF reader; `--repeat 5` alone passed.

Seen again on 2026-11-01 in the `ship` gate's `singlethreaded-clang` leg, under SGL stage-edge changes, after the same tree's `check` had passed it minutes before.

## Cause

The test filled `hits` and `ids` with zeros through `create_buffer_from_data`, binding them in the `traced` group the pictured raygen never reads.
No submitted list waits on an upload into a buffer its pipeline does not use, so the upload actor stages it when its turn comes.
An unthreaded build pumps that actor between tests' steps, and when other tests' uploads were ahead of it, the job was still pending as the test ended.
The job holds the test's context until the actor retires it, which `vulkan_upload_async_system::run_one_window` does for a job whose destination died: after nexus counted.

Found with the failing seed on `singlethreaded-clang`, about one run in four, by a build with `SC_ASYNC_AMBIENT_TRACE` on that pumps this thread while it watches.
The buffers are now made without data, so nothing is uploaded; 24 runs of the same seed passed.

Any test that uploads into a resource its GPU work never uses can hold its context this way in an unthreaded build.
