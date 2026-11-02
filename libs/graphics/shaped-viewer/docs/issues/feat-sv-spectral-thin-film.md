---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_closure.sgl
---

# The thin film is sampled at three wavelengths, so its higher orders alias

The film is sampled at three wavelengths, one per output channel, rather than integrated over the spectrum.
Its first interference order is faithful and its higher orders alias into colors the spectrum would have averaged away, so a film past roughly a micron drifts.

**It could stop aliasing now.**
The path can be collapsed onto one wavelength since dispersion landed.
Evaluating the film at the collapsed wavelength instead of at all three is most of what Belcour and Barla's spectral formulation would buy, for a path that has already paid the collapse.
Belcour and Barla's formulation is the full replacement, and it wants the same hero-wavelength machinery dispersion does — which is why the two are worth doing together.
