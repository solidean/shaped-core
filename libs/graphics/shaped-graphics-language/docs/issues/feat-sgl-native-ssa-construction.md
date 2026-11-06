---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
---

# SSA construction in the native layer

Mutable locals are function variables today, which a driver promotes.
DXIL needs them in SSA, and the optimizing passes want it.
Over the flat tables, without a per-value allocation.
