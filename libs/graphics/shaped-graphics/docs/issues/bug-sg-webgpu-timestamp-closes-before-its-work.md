---
created: 2026-11-05
---

# sg's WebGPU timestamp can close before the dispatches recorded ahead of it have run

sg's WebGPU `record_gpu_timestamp` writes its timestamp in an empty compute pass, and nothing makes that pass wait for the dispatches before it.
A timestamp pair can then close before its work has run: SGL's convolution read 0.04 ms that way in sr's WGSL benchmark, with correct output.
The benchmark times by wall clock from submit to queue completion instead; any other measurement taken with these timestamps on WebGPU may be wrong the same way.
