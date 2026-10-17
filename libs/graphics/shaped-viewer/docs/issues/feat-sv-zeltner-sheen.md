---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_microfacet.sgl
---

# The fuzz is a Conty-Estevez sheen rather than the specified Zeltner microflake

One of the three approximations named at the top of `openpbr_microfacet.sgl`, and the one that most visibly deviates.
It is a self-contained replacement.
