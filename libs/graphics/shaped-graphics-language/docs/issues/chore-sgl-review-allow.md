---
created: 2026-11-28
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/why/checking.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/linking.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_allowances.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_footprint.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/describe.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/link.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/emit.hh
---

# Philip to review `@allow` (CHK-576, CHK-577) and the link's sampler names (LINK-6)

Julius merged a change that lets an entry point say a warning is intended, and one to how a link names a sampler, made for Quest3D's SGL port of the ocean and the hybrid trace (branch `jn/sgl-allow`).
He merged it before review, so Quest3D can bake the ocean without its warnings, and asks Philip to review it: it can be adapted later where needed.

What was wrong:

- CHK-566 told an author to write `@expect(warning = "early-depth-keeps-discarded")` on a discarding early-depth pixel stage.
  CHK-403 makes an entry point with any `@expect` a test of its diagnostics, never built, so following CHK-566 removed the shader.
  `describe` did not list it, and a bake asking for it was told `no entry point named 'mainPS'`
  ([feat-sgl-allow-intended-warnings](feat-sgl-allow-intended-warnings.md)).
- Under `sgl::link`, the map named a sampler of the entry's own file by its LINK-2 name, `hits.gWater`, while the interface of that entry emitted from the link named it bare, `gWater`.
  A host building its static sampler table from the map named the slot otherwise than the stage read it ([bug-sgl-link-own-sampler-reflected-bare](bug-sgl-link-own-sampler-reflected-bare.md)).

What changed:

- **CHK-576** `@allow("kind")` on an entry point takes the warnings of that kind judged of its inlined body (CHK-402), which are then reported nowhere, and the entry point is built as without it.
  Another entry point the same warning was judged of, through a shared helper, keeps it.
  An unknown kind, an error's kind, a pattern, no argument, a named argument and an `@allow` on a non-entry function are `invalid-attribute-arguments`.
  So is a warning reported for the whole file, the seven CHK-576 lists.
- **CHK-577** An allowance is met where its warning is judged of its entry point in any compile of the file, and is `unmet-expectation` only when none raises it.
  Where a compile leaves an allowance unmet, `check::check` checks the file again (`check_allowances.cc`): for no target and the four emit targets, with the compile's own device.
  Each times every combination of the `bool` and declared-enum options the entry point reaches; a number or image-format option at the compile's value and its default.
  An allowance met so is marked `is_met_elsewhere`.
- **CHK-403** An entry point an `@expect` keeps from being built is listed in `describe`'s `unbuilt_entry_points`.
  A compile, a link or slib's package description naming it is refused as "expects a diagnostic, so it is never built".
- **LINK-6** A linked entry's interface names each sampler by its LINK-2 name, as the map does, a sampler of its own file too.
  `emit::numbering` carries each number's name; the text, the binary and every unlinked compile are unchanged.
- Tests: the corpus's `stages/early-depth.sgl`, `sgl allow` in `expect-test.cc`, and `describe-test.cc` (targets, options, a module's option behind a library layer).
  Also `link-test.cc`: the deck's water hit on dx12, vulkan, metal, SGL's own DXIL and SPIR-V, and a file naming no module.

What to look at:

- whether `@allow` beside `@expect` is the split SGL should keep, and whether its argument should stay one exact kind;
- the recheck's cost: there is no cap.
  An allowance unmet in the compile at hand checks the file again for every target and every combination of the bool and enum options its entry point reaches.
  The count grows exponentially with the options.
  That happens on every compile that leaves an allowance unmet, a truly unmet one included, and so also in the language server on every keystroke while one is unmet;
- an open point: a `needless-nonuniform` (or any of the five warnings) a test's body reaches as well stays the program's under CHK-402.
  The allowance on the entry point counts as met, while the warning still shows, reported for the program;
  whether the allowance should then be unmet, or the warning taken, is undecided;
- whether a linked entry reflecting `hits.gWater` where an unlinked compile of the same file reflects `gWater` breaks another host, and whether the unlinked compile should take the qualified name too.
