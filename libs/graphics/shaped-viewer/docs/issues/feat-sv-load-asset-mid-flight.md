---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/examples/load-asset.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/uri_resolver.hh
---

# A partially-loaded scene has still never been seen

`shaped-viewer/load-asset` closed most of this: it builds a glTF in code, serves it through `sv::set_resolve_uri` and draws what comes out.
So the loader has now been driven by a caller rather than only by a test.
What its capture does not show is the placeholders, because a synchronous load of a document this small is resident before the accumulation converges.
Seeing those wants a load slow enough to catch mid-flight, which is a resolver that stalls on purpose rather than a bigger asset.
