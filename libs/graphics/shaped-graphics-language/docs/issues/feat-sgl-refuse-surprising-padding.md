---
created: 2026-11-22
priority: next
tags: [porting-feedback]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# Padding a host would not expect is refused until it is spelled out

`buffer[float3]` strides by 16 without a word (EMIT-111), where a host that writes `tg::vec3f` strides by 12.
A port lost a debugging session to it.
The generated host struct is right (`slib::gpu_padded<tg::vec3f, 16>`), but a host that hand-writes its upload never sees it.

**The rule wanted is a meta rule: padding that would surprise is never implicit.**
Where the natural layout (EMIT-110) places memory other than a C++ compiler places the same types, the compile is refused until the binding or member says which it means.
`@padding(.none)` is one answer, and the natural layout needs an explicit spelling of its own, such as `@padding(.natural)`.
The diagnostic names the place, both sizes and both fixes.

Open:

- Where the boundary is: a buffer's element stride surely, and a `mat3`'s 48 bytes and a struct member after a scalar probably; a constant block's row rules are the question.
- `@padding` on a struct is `unsupported-yet` (emitting.md's Open list), and is what makes the annotation cheap where one struct is used by many bindings.
