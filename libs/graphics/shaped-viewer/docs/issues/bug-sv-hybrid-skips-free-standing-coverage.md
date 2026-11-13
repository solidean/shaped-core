---
created: 2026-11-04
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.cc
  - test: sv agreement - every kind of content draws the same in every mode
---

# The hybrid's rays pass through free-standing geometry whose coverage is a fraction

An instance whose material keeps its coverage as a fraction and lights nothing gets `instance_mask_hybrid_skips`, so every hybrid ray passes it.
That is a decal's case, a soft mask on a surface whose shadow and bounce barely move, and it halved Sponza's lighting pass.
A free-standing one is not a decal: a curtain, a soft-alpha fence, a panel of partial coverage casts no shadow in the hybrid, and its bounce light is missing.

Measured in the content agreement test, a panel standing in the lit Cornell box, the hybrid against the megakernel one bounce longer:
- with coverage 0.5 everywhere, the panel's region is 3.4% too bright forward and 2.3% deferred, 5 to 7 standard errors;
- with coverage 1 everywhere, still kept as a fraction, 37.5% too bright in both, since the rule reads the material's coverage mode rather than its values.
The tracers agree with each other in both, and a thresholded cutout (`alpha_cutoff`) agrees in every mode, so that test draws its panel thresholded.

Two refinements are open, neither decided:
- skip only where the coverage really is fractional, which a constant or an attribute whose values are all 1 says on the CPU;
- tell a decal from a free-standing surface, by whether the instance lies on another's surface, which is the decision that matters.
