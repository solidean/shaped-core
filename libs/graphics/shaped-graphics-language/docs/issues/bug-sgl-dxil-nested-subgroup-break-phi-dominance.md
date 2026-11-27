---
created: 2026-11-26
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer.cc
---

# SGL's own DXIL of a subgroup loop with a break inside another loop fails the validator: a phi does not dominate its uses

DXC's validator refuses SGL's own DXIL of this compute stage with `Module bitcode is invalid. Instruction does not dominate all uses! %47 = phi float ...`:

```sgl
require subgroups

binding io:
    source: buffer[float]
    target: mut buffer[float]
    n: int
    limit: float

@compute(64) fun cs(@thread_id id: int3){io}:
    let mut total = 0.0
    for k in 0 ..< io.n:
        for i in 0 ..< io.n:
            total += subgroup_add(io.source[i + k])
            if total > io.limit => break
            if io.source[i] < 0.0 => continue
            total += 1.0
    io.target[id.x] = total
```

Found by the review of EMIT-291 on 2026-11-26, through slib's validator; it reproduces with no `@unroll` attribute, and such a module writes no loop metadata, so it predates that change.
The same loop alone, not nested, passes.
Hypothesis, unverified: the split blocks of EMIT-279's `@dx.break.cond` break leave a phi of the outer loop without the edge it needs.
