---
created: 2026-10-30
closed: 2026-10-30
resolution: fixed
---

# An `unsupported-yet` of the flat tree names no construct, and points at the entry point

`main: its body reaches a construct the flat tree cannot hold yet` is reported at the entry point's name, `wg.sgl:4:18`, however deep the construct is.
A reader has to bisect the body to find it, which is how `feat-sgl-compound-assign-workgroup-element` was found.

What is wanted: the diagnostic at the construct itself, naming it (`op=` on a workgroup element), with the entry point as a related note when it was reached through an inlined call.
