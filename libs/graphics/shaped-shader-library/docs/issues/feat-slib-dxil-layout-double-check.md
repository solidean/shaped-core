---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/compiler/dxc_compiler.cc
---

# The SGL layout double check on DXIL

slib compares what SPIR-V and WGSL place against what SGL states, and reads no DXIL layout: the bytecode carries no reflection container to read it from.
dx12's packing is SGL's own rule, so this is the target least likely to disagree.
A DXIL arm would need the container kept beside the bytecode.
