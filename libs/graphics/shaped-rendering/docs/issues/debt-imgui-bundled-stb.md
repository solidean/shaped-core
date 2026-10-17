---
created: 2026-10-17
parent: epic-sr-imgui-backend-gaps
refs:
  - file: libs/graphics/shaped-rendering/CMakeLists.txt
  - file: libs/graphics/shaped-rendering/docs/slug.md
---

# Get imgui off stb

imgui bundles stb rect-pack, truetype and textedit.
We scope them with `IMGUI_STB_NAMESPACE` so they cannot collide with anyone else's stb, but scoping is containment, not a fix.
stb is hobby-grade — no release process, known robustness gaps parsing malformed fonts — and it sits on the path that loads user-supplied font files.

Two ways out:
- upstream's freetype backend (`misc/freetype`), a second vendored dependency;
- our own rasterizer.

[slug.md](../slug.md#why-not-something-else) weighs the same two against each other for Slug.
