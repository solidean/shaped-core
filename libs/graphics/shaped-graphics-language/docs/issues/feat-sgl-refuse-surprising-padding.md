---
created: 2026-11-22
priority: next
tags: [porting-feedback]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
---

# A buffer of a builtin padded past its host type is refused until it says so

`buffer[float3]` strides by 16 without a word (EMIT-111), where a host that writes `tg::vec3f` strides by 12.
A port lost a debugging session to it.

**The meta rule: padding that would surprise is never implicit.**
It binds where a builtin type's GPU layout differs from its natural C++ host type, since that is where a host writes its own data.
A user struct does not need it: the host writes the generated struct, which carries the padding already.

So the refusal covers a buffer whose element is a builtin, or an array of builtins, whose stride is not the host type's size.
`buffer[float3]` (16 against `tg::vec3f`'s 12), `buffer[half3]` (8 against 6), and `buffer[mat3]` (48 against `tg::mat3f`'s 36) are the cases known.

- The fixes are `@padding(.none)`, which gives the host type's stride, and an explicit spelling of the natural layout, such as `@padding(.natural)`.
- The diagnostic names the element, both strides and both fixes.
- `buffer[float4]`, `buffer[float]` and every other builtin whose stride is its host type's size stay as they are.
