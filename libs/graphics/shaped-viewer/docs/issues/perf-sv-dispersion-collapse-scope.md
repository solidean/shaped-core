---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_evaluate.sgl
---

# Dispersion collapses the whole path onto one wavelength, and keeps it collapsed

Everything after the collapse costs three times the samples for the same noise, including surfaces that have nothing to do with the dispersive one.
Collapsing only the transmitted lobe's continuation — leaving a reflected one carrying all three — is what would bound that.
It needs the closure to say which lobe a sample came from beyond the interior it entered.
