---
created: 2026-11-14
refs:
  - test: "sv agreement - every denoiser keeps the image's brightness and brings it closer to the converged one"
  - test: "sv agreement - the comparison tells the Cornell box's direct light from its full transport"
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/pipeline_cache.cc
  - issue: perf-sg-raster-pipelines-never-persist
---

# One shaped-viewer run hung the GPU on the first run that restored raster pipelines from the persistent tier

Seen once (2026-11-14, RTX 4090, driver 32.0.16.1088), in the first `--no-server` run after raster pipelines gained the persistent tier, which restored the blobs the run before it had stored.
Two tests ran about 120 s each, from 37 s and from 156 s, with nothing recorded on the CPU between 30 s and 150 s.
At 281 s the device was removed with DXGI_ERROR_DEVICE_HUNG, right after a hybrid stream's guides read back non-finite at frame 0.
Seven runs since, with the same store and the same blobs restored (134 per run), all passed in 46–52 s.

Hypothesis, not verified: a restored pipeline ran another pipeline's code, as the static-sampler bug does on this driver (docs/bugs-external/d3d12-cached-pso-static-sampler-mixup).
That bug needs a blob restored in a process that first built a twin fresh, which fits a first run restoring fresh blobs.

The run's logs and its recording are in a consumer checkout's gitignored crash folder, `shaped-viewer-test-20261114-183029-failed/`.
`sg.pipeline.cached_blob_used` says which creates restored; the first step is a run that logs each restored pipeline's key, to name the one that hangs.
