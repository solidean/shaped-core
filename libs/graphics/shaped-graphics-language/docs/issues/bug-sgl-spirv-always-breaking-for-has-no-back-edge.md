---
created: 2026-11-26
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/flatten.cc
---

# SGL's own SPIR-V of a for whose body always breaks has a loop header with no back edge

spirv-val refuses SGL's own SPIR-V of this compute stage:

```sgl
binding io:
    source: buffer[float]
    target: mut buffer[float]

@compute(64) fun cs(@thread_id id: int3){io}:
    let mut total = 0.0
    for i in 0 ..< id.x:
        total += io.source[i]
        break
    io.target[id.x] = total
```

`Loop header '%39' is targeted by 0 back-edge blocks but the standard requires exactly one`.
Measured 2026-11-26 with `sgl emit --target spirv` and `spirv-val --target-env vulkan1.3`, with and without `@unroll(false)` on the loop, so it predates EMIT-291.
The review of EMIT-291 also saw the loop's header joined into the function's entry block.
Hypothesis, unverified: the body's `break` leaves the continue block unreachable, and a structured pass drops the back edge SPIR-V requires, which `once` keeps with a `branch_if(false, ...)`.
