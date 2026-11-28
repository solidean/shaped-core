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

## Follow-ups decided by Julius (2026-11-28)

1. **An allowance is judged across targets and option values.**
   An `@allow` is met when its warning arises for any target or option value the file can be compiled for, and unmet (CHK-577) only when none raises it.
   Before, an allowance whose warning stands in a `const if target_language == .hlsl:` branch was unmet in every compile for another target and in `describe`:

   ```sgl
   @early_depth_test
   @allow("early-depth-keeps-discarded")
   @pixel fun mainPS(input: VSOutput) -> float4:
       const if target_language == .hlsl:
           if input.coverage < 0.5:
               discard
       return shade(input)
   ```

   Options are substituted where their const is compiled, so judging other option values means checking the file again with them.
   Julius chose how (2026-11-28), and CHK-577 now says it:
   - Where the compile at hand meets no warning of an allowance, `check::check` checks the file again until a compile meets every allowance, or none is left.
     An allowance met so is marked `is_met_elsewhere`, and `test::contain_expected` reports nothing for it.
   - The targets are no target and the four `emit::all_targets` writes for (`check::every_compile_target`), all with the compile's own device, as a link gives one device to every target.
   - Each with every combination of the values of the `bool` and declared-enum options the allowing entry point reaches (CHK-355), each entry point on its own.
     A number or image-format option stays at the compile's value and is tried at its default too.
   - No cap; to revisit if it ever costs too much.
     A check from a layer that cannot serve another value of a module's option checks the files instead.
     An allowance is matched across those checks by its entry point's qualified name, place and kind, since file positions differ.
   Tests in describe-test.cc: the HLSL-branch cutout built on every target and described, and a branch no target takes still unmet.
   Also a `bool` option, a declared enum with a target, and a number met at its default and unmet where no tried value raises it.
2. **A warning reported for the whole file is refused where it is allowed** (done, CHK-576).
   Only `early-depth-keeps-discarded`, `nan-test-at-fast`, `needless-nonuniform`, `unroll-too-long` and `workgroup-wastes-lanes` are judged of an inlined body.
   `@allow` of any other warning is `invalid-attribute-arguments`, "'no-effect' is reported for the whole file, a pipeline or a generic, not for an entry point, so no entry point can allow it".
   Those are the parser's `spaced-attribute-arguments`, the AST pass's `redundant-yield` and `no-effect`, and a body's own `unreachable-code`.
   So are the file's `unused-require`, a generic's `generic-not-instantiated` and a pipeline's `pixel-stage-ignores-view`.
   A warning a test's body reaches as well stays the program's (CHK-402), so no allowance meets it; CHK-576 now says so.
   Tests in expect-test.cc: each of the seven refused, every other warning accepted, each of the five met by an allowance, and a test-reached `unroll-too-long` unmet.
