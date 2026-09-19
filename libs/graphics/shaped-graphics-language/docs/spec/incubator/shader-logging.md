# Shader Logging and Assertions

*Incubator: not normative.*

## The idea

SGL has no first-class strings and does not want them.
What it wants is **compiler-supported `print` and `assert` with full string interpolation** — and that is much easier than first-class strings, because it is compiler-internal.

**The mechanism.**

* Compiling with log and assert support on adds a few buffers to the reserved third binding group.
* A `print` or a failing `assert` emits shader code that reserves slots and a payload region in those buffers with an atomic add.
  When the buffers are exhausted the reservation is a no-op, and an `assert` still returns or discards on failure.
* `sg` drains the buffers on the CPU and acts on them.
* The static pieces of an interpolated string live in **metadata**, not on the GPU.
  Only the payload — the interpolated values — is written per invocation.
  The final string is reconstructed and formatted on the CPU.
* Because every function is called statically ([function-model.md](function-model.md)), each site has a **fixed source id**.
  That gives exact stack traces after the fact, with no runtime cost beyond the id.

Compiled without support, `print` and `assert` vanish entirely.
That is why they are keywords rather than builtin functions: a reader must be able to see that they are special and may compile to nothing.

```sgl sketch
print "pos: $p.x, $p.y and $(length(p) * 2)"
assert n_dot_l >= 0.0, "negative cosine for normal $n"
```

The same machinery serves the testing keywords the readme names, `CHECK` and `REQUIRE`, and per-expression traces.

## What it touches

* The binding model: the third binding group is reserved for the toolchain.
* `sg`: allocation, draining and decoding of the log buffers, and surfacing results into `cc::rec`.
* The transpiler: the atomic reservation must be expressible in every target, which is a question for WGSL in particular.
* The metadata format that carries string pieces, source ids and payload layouts.

## Already fixed by the syntax

* Interpolation is `$name`, `$name.member`, `$(expr)`, with `$$` for a literal dollar, in double-quoted strings only.
  It is tokenized from the first day, so no string changes meaning when this lands.
* A keyword form takes comma-separated expressions, which is what gives `assert cond, "message"` its shape.
* `print` and `assert` are keywords.

## Open

* What a payload may contain: scalars and vectors certainly; structs, arrays and matrices by flattening.
* Format specifiers inside an interpolation, such as precision or hex.
* Ordering and deduplication across invocations: a pixel shader can log millions of times per frame.
* Filtering at the source, for example "only for this pixel" or "only the first N", and whether that is syntax or API.
* Behaviour on targets without atomics in the required stage.
* Whether an `assert` failure discards, returns a sentinel, or is configurable per stage.
