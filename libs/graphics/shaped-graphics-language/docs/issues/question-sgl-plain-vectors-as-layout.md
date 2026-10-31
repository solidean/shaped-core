---
created: 2026-10-31
---

# floatN and intN as permissive layout types

`vec3 as float3`, `pos3 as float3` and `hpos4 as float4` have landed, the way out of a strong type to the plain vector of its width.
The maintainer's model goes further: a plain vector works on the unrolled member layout of a value, and is permissive about where that layout comes from.
Each point below is a direction to talk through, not a decision.

- **A constructor takes a strong vector without a splat**: `float4(normal * 0.5 + 0.5, 0.0)` rather than `float4(..(normal as float3) * 0.5 + 0.5, 0.0)`.
  This makes the plain-vector constructors special cases of their own.
- **`vec3` and `pos3` with scalar `*` and `+`**: `normal * 0.5 + 0.5` then stays a `vec3`, which is the encoding ports write.
  A `pos3 + 0.5` has no geometric meaning, which is the question.
- **A struct whose members, unrolled in order, are all `float`, converts with `as float3`**, nested structs included, and stands in a constructor as `float4(v, 0.0)`.
- **The other direction**, `some_float3 as foo`, where `foo`'s unrolled members are three floats.
- What the strong types are for if every plain vector converts both ways; the answer probably keeps the strong-to-strong conversions explicit.
