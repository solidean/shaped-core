---
created: 2026-11-08
parent: epic-sgl-native-ir-targets
refs:
  - file: libs/graphics/shaped-rendering/shaders/compression/bc.sgl
  - file: libs/graphics/shaped-graphics-language/docs/native-parity.md
---

# bc7's DXIL is 1.27 times DXC's: divisions by constants, selects and loads

After `state_constant_tables`, bc7's DXIL holds 8063 operations against DXC's 6363, and its stores match DXC's.
`native-parity.py diff` on `bc7_cs`, on 2026-11-08, the largest differences:

| operation | native | DXC |
|---|---:|---:|
| `load` | 590 | 348 |
| `select` | 405 | 171 |
| `fmul` | 627 | 415 |
| `fdiv` | 260 | 65 |
| `dx.binary` | 504 | 344 |
| `phi` | 927 | 823 |

Measured: the counts.
Hypothesis, from the shape: DXC writes `x / c` as `x * (1 / c)` under HLSL's default fast math, which `.fast` allows SGL too.
The wavefront's shade kernel shows the same `fdiv` surplus, so one fold may serve both.
The extra loads are not one table read twice: `share_common_values` shares a table's reads, and doing so changed no count.
bc7's SPIR-V is 0.99 of DXC's, so whatever this is, it is in what DXIL's route does after the shared passes or in how it writes.
