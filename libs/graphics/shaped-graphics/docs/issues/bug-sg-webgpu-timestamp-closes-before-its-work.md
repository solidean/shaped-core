---
created: 2026-11-05
---

# sg's WebGPU timestamp can close before the dispatches recorded ahead of it have run

sg's WebGPU `record_gpu_timestamp` writes its timestamp in an empty compute pass, and nothing makes that pass wait for the dispatches before it.
A timestamp pair can then close before its work has run: SGL's convolution read 0.04 ms that way in sr's WGSL benchmark, with correct output.
The benchmark times by wall clock from submit to queue completion instead; any other measurement taken with these timestamps on WebGPU may be wrong the same way.

## 2026-11-05: timestamps are pass ends and beginnings now, and two problems remain

A timestamp is now the end of the pass just before it, or the beginning of the next, which is all WebGPU promises; the empty pass is left only where no pass comes.
Timed that way on an Arc B580, à-trous and SVGF's filter agree with the wall clock on Dawn over D3D12.
SGL's convolution still reads 0.14 ms a dispatch over 128 against the wall clock's ~1 ms.
With one dispatch a pair its minimum is 0, so that pass's own timestamps do not bracket its dispatch there.
Both contenders record the same commands and differ only in the shader, so the cause is in Dawn, D3D12 or the driver, and is not found yet.
On wgpu (deno) every value is about 60× too small, so its ticks are not nanoseconds as sg assumes, or the period is applied twice.
sr's WGSL benchmark keeps timing by wall clock until both are understood.

