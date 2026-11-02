---
created: 2026-10-28
---

# An example that fetches gltf-research-scenes and renders them

**Wanted: an example that fetches the scenes of https://github.com/ErfanMo77/gltf-research-scenes on request and renders them**, one picked at a time.
Asked for on 2026-10-28, after the hybrid renderer's current work; later to render them in studio mode too, [feat-sv-studio-mode](feat-sv-studio-mode.md).

What is known:

- 27 scenes, most converted from Benedikt Bitterli's PBRT scenes, each as `*_core.gltf` and `*_extended.gltf`; release v1.1.0 packs them as `.glb`.
- **Only the CC0 and CC-BY-3.0 scenes are offered**, decided on 2026-10-28; the four of mixed license are left out.
  - CC0: bathroom, bmw-m6, classroom2, cornell-box, cornell-caustic, glass-of-water, house, lamp, pontiac-gto, spaceship, veach-ajar, veach-bidir, veach-mis.
  - CC-BY-3.0: breakfast-room, bathroom2, classroom, coffee-maker, dragon, kitchen, living-room, living-room-2, modern-hall, staircase; the script prints the attribution, and the example shows it.
- **Fetched on request, never by a build**, as `extern/sponza/fetch-sponza.py` fetches Sponza: the user accepts each license themselves.
- What the `_extended` scenes use that sv's importer reads as an issue rather than a feature is not checked yet.
