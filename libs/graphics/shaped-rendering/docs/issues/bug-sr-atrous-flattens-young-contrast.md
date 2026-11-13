---
created: 2026-11-03
refs:
  - file: libs/graphics/shaped-rendering/shaders/atrous_denoise.sgl
  - test: sv agreement - every denoiser keeps the image's brightness and brings it closer to the converged one
---

# À-trous darkens a young image and flattens its contrast

**From four samples on, this is fixed**: sv's mean carries its moment, and à-trous steers by the measured variance with a window that is the pair's.
At four samples it is within 0.4% of the converged brightness behind the megakernel and the hybrid, its worst tile within 7%, which the agreement test now holds it to.
**Below four samples it remains**: the variance is modeled there, and inside a cluster of black samples every modeled variance is small, so the cluster stays dark.
The mode-switch fuzzer finds a one-sample frame 5.3% dark against its 5% ceiling, and exempts à-trous below four samples under this issue.
SVGF's spatial estimate, the variance over 7x7 neighbors while the history is young, is the remaining fix, in a pass of its own so the first filter pass need not repeat it per tap.

What follows is the defect as it stood before that.

On the Cornell box at four samples a pixel, à-trous comes out 8% darker than the raw mean behind the megakernel and 15% behind the hybrid.
It also moves light from the lit back wall into the ceiling's corners, a fifth of the frame's mean brightness between 16x16 tiles.
SVGF, OIDN and DLSS-RR keep the brightness within 2% and move at most 14% between tiles.

The darkening is the luminance window, sized by the center pixel alone: `scale * (|center| + 0.01)`.
A dark pixel's window is narrow and refuses the bright samples beside it, while a bright pixel's is wide and takes the dark ones in.
So energy only ever moves downward.

Sizing the window by the brighter pixel of each pair removed the darkening, and was reverted.
Such a window can never refuse a tap brighter than the center, so a one-sample frame's brightest sample spread over half the image, a block that jumped from frame to frame.
`sr - atrous keeps a single extreme sample where it fell` holds that now.

A fix has to keep refusing outliers while letting a dark pixel take in its share of bright noise.
The member departs from its reference here: Dammertz et al. 2010 weigh a fixed sigma per pass, halved each pass, symmetric in the difference.
The center-relative window and its `1 / sqrt(samples)` scale are sr's own noise model.

The published ways to a width that is measured rather than modeled:
- SVGF (Schied et al. 2017): the variance of the mean from per-pixel luminance moments, 3x3-blurred, a 7x7 spatial estimate while the mean is young; `svgf.sgl` has it.
  It needs a second moment the accumulated mean does not keep: sv would accumulate it, in the mean's unused alpha.
- Rousselle et al. 2012: per-pixel variance from two half buffers of the same mean, which needs no moment but a second accumulation.
- Either wants an anti-firefly clamp first, as NVIDIA's ReLAX applies, or one extreme sample widens its own window and spreads.
`feat-atrous-measured-variance` is the issue for that work.
The flattening is measured and not explained yet; the same noise model, letting a stray bright sample near the lamp spread along the ceiling, is the likely cause.
