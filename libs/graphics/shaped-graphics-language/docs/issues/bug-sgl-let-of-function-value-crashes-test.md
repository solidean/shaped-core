---
created: 2026-11-22
priority: next
refs:
  - file: libs/graphics/shaped-graphics-language/tests/corpus/calls/function-values.sgl
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-24T14:28Z
---

# A let of a function value, called, crashes sgl test

`sgl test` of a file holding the function below fails with `index out of bounds at sgl.exe+0x2a8dcd` once a test calls `apply`:

```sgl
fun apply(f: (float) -> float, x: float) -> float:
    let g = f
    return g(x)
```

Found while adding `let` of resources (CHK-536); it reproduces with that change stashed, so it predates it.
Passing `f` on directly, `return f(x)`, does not crash.
Hypothesis: the flattener binds `g` to a local where a function value has no flat local, the case CHK-536 gave resources a stand-in for.
