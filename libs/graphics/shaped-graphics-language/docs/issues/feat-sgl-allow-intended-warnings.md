---
created: 2026-11-28
priority: next
tags: [sgl]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
  - symbol: sgl::test::contain_expected
  - symbol: sgl::check::impl::checker::read_allowances
  - file: libs/graphics/shaped-graphics-language/tests/corpus/stages/early-depth.sgl
---

# `@allow("<warning>")` on an entry point, and an `@expect`ing entry point named as never built

CHK-566 told an author to write `@expect(warning = "early-depth-keeps-discarded")` on a discarding pixel stage under `@early_depth_test`.
CHK-403 makes an entry point with any `@expect` a test of its diagnostics, never built.
So following CHK-566 removed the shader, and `describe` listed no such entry point.
A bake asking for it was told `no entry point named 'mainPS'`, and a library check compiling the described entry points passed without ever compiling it.
Found porting Quest3D's ocean, whose four pixel stages port HLSL's `[earlydepthstencil]` with a `discard`.

Repro:

```sgl
require early_depth_test

struct VSOutput:
    @position position: hpos4

@vertex fun mainVS(@vertex_index vertexId: int) -> VSOutput:
    return {position = hpos4(0.0, 0.0, 0.0, 1.0)}

@early_depth_test
@expect(warning = "early-depth-keeps-discarded")
@pixel fun withExpect(input: VSOutput) -> float4:
    if input.position.x < 0.5:
        discard
    return float4(1.0)
```

`sgl emit --entry withExpect` gave `error: no entry point named 'withExpect' (the source holds: vertex 'mainVS')`.

Decided by Julius (Quest3D's sgl-migration-readiness plan, D110 item 1): `@expect` stays a test of diagnostics, and a new `@allow` says on a built entry point that a warning is intended.

## Resolution (branch `jn/sgl-allow`, under review)

Julius and Philip Trettner review the branch before it merges.

- **CHK-576** `@allow("kind")` on an entry point, of any stage, takes the warnings of that kind judged of its inlined body (CHK-402), which are then reported nowhere.
  The entry point is built as without it.
  Another entry point the same warning was judged of, through a shared helper, keeps it.
  A kind no diagnostic has, an error's kind, a pattern, no argument, a named argument and an `@allow` on a non-entry function are `invalid-attribute-arguments`.
  An allowed warning is nothing an `@expect` meets.
- **CHK-577** An allowance no warning meets is `unmet-expectation`, a diagnostic of its entry point alone; an entry point never built or whose body did not check reports none.
- **CHK-566** now points to `@allow`.
- **CHK-403** gained: such an entry point is still the file's, and `describe` lists it in `unbuilt_entry_points` (`expects a diagnostic, so it is never built`).
  A compile or a link naming it is refused in those words.

Tests: the corpus's early-depth file, with an allowed cutout built for every target with the feature, and a helper's warning allowed by one stage and expected by another.
Also `sgl allow` in expect-test.cc, and `sgl describe` and `sgl link` tests.
Once merged, Quest3D's ocean.sgl takes `@allow` and its four bake warnings go.
