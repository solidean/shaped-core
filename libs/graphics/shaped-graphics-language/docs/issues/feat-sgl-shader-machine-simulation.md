---
created: 2026-11-05
parent: epic-sgl-shader-disassembly
---

# Simulate a shader on a GPU model: register pressure, occupancy and cycle estimates

Later than the disassembly: a model of a GPU that estimates what an entry point costs, so an optimization is judged before it runs on hardware.
Vendors' static analysis is the start, Radeon GPU Analyzer's register and occupancy figures above all; a cycle-level model is the far end.
It is what would let an agent optimize a shader against a target it does not have, as the CPU side's instruction tracer lets it read what code ran.
