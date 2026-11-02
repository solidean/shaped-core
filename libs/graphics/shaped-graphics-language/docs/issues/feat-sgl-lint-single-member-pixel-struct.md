---
created: 2026-10-31
---

# A lint that turns a single-member pixel struct into a direct result

A `@pixel fun` may return its one render-target value directly, annotated, rather than through a `@pixel struct` of one member.
Ported code still writes the struct, since HLSL and GLSL ports start from one.

- **Wanted**: a style lint, with an autofix, that replaces a `@pixel struct` of a single member with the direct result.
- **Only where the struct is used once**: a struct several entry points share, or one a function builds, stays.
- The fix rewrites the return type, each `return { m = v }` to `return v`, and deletes the struct.
