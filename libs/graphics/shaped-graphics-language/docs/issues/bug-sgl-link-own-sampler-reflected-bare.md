---
created: 2026-11-28
tags: [sgl]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/linking.md
  - symbol: sgl::link_result::emit
  - test: sgl link - a sampler of the entry's own file is reflected by its map name, on every route
---

# A sampler of a linked entry's own file is reflected bare, while the link map names it by LINK-2

Under `sgl::link`, the map names a file-scope sampler declared in the entry's own file by its module, `hits.gWater`, or by the file's path where the file names no module, `trace.sgl:gBorder` (LINK-2).
The interface of that entry emitted from the link (`emitted_source::bindings`) named it bare, `gWater`, as an unlinked compile does.
A host building its pipeline layout's static sampler table from the map then names the slot otherwise than the stage reads it, and a fit check by name refuses the pipeline.
One sampler could even carry two names inside one group: bare in its own file's stages, `hits.gWater` in a stage of another file that uses `hits`.
Found wiring the link into Quest3D's bake (its SGL-Wire notes); the deck's `hits.sgl:water` in link-test.cc shows it.

Decided by Julius (Quest3D's sgl-migration-readiness plan, D110 item 3): under a link the sampler is reflected by its LINK-2 name, and unlinked compiles keep today's names.

## Resolution (branch `jn/sgl-allow`, under review)

Julius reviews the branch before it merges.

LINK-6 gained the sentence: an entry's interface names each sampler by its LINK-2 name, as the map does, a sampler of the entry's own file too.
The text spells it as the file compiled alone does.
`emit::numbering` carries each number's LINK-2 name, and the interface of a linked emit takes it; the text, the binary and an unlinked compile are unchanged.
Tests: the deck's water hit on dx12, vulkan and metal text and SGL's own DXIL and SPIR-V, a file naming no module on every target, each against the map.
