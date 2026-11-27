---
created: 2026-11-26
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/why/checking.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_raytracing.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/describe.cc
---

# Philip to review the callable section (CHK-562)

Julius merged a change to how a callable call's index is computed, made for Quest3D's SGL port of the hybrid trace (branch `jn/sgl-callable-index`).
He asks Philip to review it.

What was wrong:
`table[i](mut c)` took its table's place by counting every table earlier in the whole library's module chain, unreached modules included.
It counted differently on each of the three check paths, while `describe` counted only the reached tables.
One unrelated library module with a table shifted a pipeline's indices past its callables.

What changed:

- CHK-562 defines a program's **callable section**: the tables of the modules it reaches, its own included.
  A used module's tables come before its user's, ties go by module name, the program's own module comes last, and a module keeps its declaration order.
- `checker::order_callables` builds it once per check, from the `module` and `use` lines and the names alone.
  The call (`flatten_callable_call`), the `.host` rule, the pipeline's callables list and `describe` all read it.
- `describe` lists every table of the section with its offset and entries, names qualified by module (`light.picks`).
  The pipeline's callables list and frozen line use the same names.
- Tests: `tests/driver/callable-section-test.cc`, and a GPU test in shaped-graphics' `raytracing-pipeline-test.cc` on dx12, native DXIL and vulkan.

What to look at:

- whether the order (dependencies, then names) is the one SGL should promise;
- whether `describe`'s qualified names break another host;
- the gap the change leaves (issue `feat-sgl-build-reached-modules-callables`): a used module's callables are listed in the section but not built in the program that reaches them (CHK-350).
- the link step that closes that gap (branch `jn/sgl-link`): CHK-562 gains the sentence that a link group's numbering replaces the program's.
  A new page, [linking.md](../spec/semantics/linking.md), holds LINK-1 to LINK-9.
  A flattened callable call now keeps `i + callable_base(t)`, resolved when the entry is written (LINK-5), so an unlinked compile writes what it wrote.
