---
created: 2026-11-22
priority: normal
tags: [porting-feedback, dx12]
---

# A dx12 shader creates a descriptor handle per access to a binding array's element

A port measured about 1.7 times the `dx.op` count of the hand-written HLSL in a TAA pass, which it attributes to one handle per binding-array access.
That is a reported measurement, not one made here, and nothing measured whether drivers merge the handles.

To do, in order:

1. Reproduce with `uv run dev.py run shan -- diff` of SGL's DXIL against DXC's of the hand-written HLSL, on a shader that reads one element twice.
2. Say whether the extra handles come from SGL's text, from DXC declining to merge them, or from SGL's own DXIL.
3. If it is the text, emit one local per distinct element within a block, the index evaluated once.
