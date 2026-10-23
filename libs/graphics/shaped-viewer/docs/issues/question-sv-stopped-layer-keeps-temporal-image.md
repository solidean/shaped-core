---
created: 2026-10-18
parent: epic-sv-denoising-integration
---

# A layer that stops during the temporal phase keeps its last temporal image

At its target or at `accumulation_sample_cap` a frame traces nothing, so the frame count stops and the hand-off with it.
That is under `temporal_denoise_frames + temporal_denoise_fade_frames` frames, so at more than about 680 `samples_per_pixel` on the megakernel, or a low capture target.
Running the temporal member on would feed it the last frame's samples again as fresh ones.
If the spatial member on the finished mean turns out wanted there, one spatial-only denoise once the layer stops is the fix.
