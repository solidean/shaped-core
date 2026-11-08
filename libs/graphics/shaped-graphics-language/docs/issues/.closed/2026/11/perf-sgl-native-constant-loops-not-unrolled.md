---
created: 2026-11-07
priority: next
parent: epic-sgl-native-ir-targets
refs:
  - file: libs/graphics/shaped-graphics-language/docs/native-parity.md
  - file: libs/graphics/shaped-rendering/shaders/compression/bc.sgl
closed: 2026-11-08
resolution: obsolete
---

# The native route unrolls no loop by itself, so a local array a loop indexes stays in memory

DXC fully unrolls a loop of constant bounds and few iterations, then promotes every local array the loop indexed to registers.
The native route unrolls only what `@unroll` asks for (CHK-389), so such an array stays an alloca: scratch memory on a GPU, a load and a store per access.
That is most of what is left of bc7's DXIL gap: 67 allocas against DXC's 27, 1156 stores against 460, 9507 operations against 6363.
The viewer's environment loops had the same cause, which `@unroll` on them fixed: path_miss's DXIL went from 1209 operations to 412, DXC's being 397.
The fix is a pass on the IR between construct_ssa and simplify: unroll a loop whose phi counts from a constant by a constant to a constant, within a copy limit.
Then promote the locals that became constant-indexed.
It belongs in ir/ rather than in flatten, since CHK-389 is the language's `@unroll` and its diagnostics, and an unroll the compiler chooses changes no text a target receives.
docs/native-parity.md's table is how to see it close: bc7 on both writers.

## Tried on 2026-11-08, and shelved

An IR pass between `construct_ssa` and `simplify` unrolled each `for` of constant bounds with no `break` or `continue`, then promoted the arrays that became constant-indexed.
It was correct: the corpus, sg, sr, slib and sv passed on the B580 with it.
It never shrank an entry, over all 106 of `native-parity.py table`, native operations over DXC's:

| entry | before | every short loop | only loops indexing a local by their counter, 256 operations at most |
|---|---:|---:|---:|
| `bc7_cs` dxil | 1.49 | 1.67 | 1.50 |
| `bc4_cs` spirv | 1.07 | 8.16 | 1.44 |
| bloom `first_cs` spirv | 1.01 | 8.50 | 1.01 |
| à-trous `main_cs` spirv | 1.02 | 3.11 | 1.02 |

DXC keeps these loops, so the premise above was wrong.
bc7's gap was its constant tables, which a local filled with a store per element; `state_constant_tables` closed that part, from 1.49 to 1.27.
Should an unroll come back, it wants a trial: unroll, clean up, and keep it only where the program shrinks.
