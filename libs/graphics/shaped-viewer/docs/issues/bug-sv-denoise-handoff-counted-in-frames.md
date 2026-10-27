---
created: 2026-10-18
parent: epic-sv-denoising-integration
---

# A denoised capture's hand-off from the temporal member to the spatial one is counted in frames

`temporal_denoise_frames` and the fade are frames of the mean, and a wavefront frame holds as many samples as the GPU finished.
So where a capture taken at N samples per pixel lands in the hand-off, and so what it denoised with, depends on how fast the GPU is.
Counting the hand-off in samples is the fix; no committed capture denoises yet.
