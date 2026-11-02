---
created: 2026-10-29
---

# An image environment for HDRIs, behind the environment interface studio mode introduces

**Wanted: an environment kind that is an image**, an equirectangular HDRI sampled by its radiance, beside the SH probe and the studio dome.
Split out of [feat-sv-studio-mode](feat-sv-studio-mode.md) on 2026-10-29, whose dome is analytic and needs no image.

What is known:

- Studio mode puts every miss, environment sample and MIS weight behind one interface: radiance along a direction, a sampled direction with its pdf, and the pdf of a direction.
  This is a third kind behind it, so no caller changes.
- The likely shape: an `rgba16f` equirectangular texture and a 2D sampling table, marginal and conditional CDFs over its rows and columns, weighted by `sin θ`.
- Bistro ships one, `san_giuseppe_bridge_4k.hdr`, which its example cannot use today; babel reads Radiance HDR already.
- A visible environment and a lighting one may differ, as studio mode's backdrop does, so the kind carries what a camera ray sees separately.
