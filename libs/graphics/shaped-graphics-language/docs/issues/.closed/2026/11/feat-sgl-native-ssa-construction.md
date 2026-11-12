---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
closed: 2026-11-06
resolution: fixed
---

# SSA construction in the native layer

Mutable locals are function variables today, which a driver promotes.
DXIL needs them in SSA, and the optimizing passes want it.
Over the flat tables, without a per-value allocation.

## Done

`ir::construct_ssa` runs on both routes, a local splitting into one value per scalar where every access is whole or at constant indices.
A local an index reaches at run time stays a variable.
