# What SGL wants from the lower libraries

The repo rule is that a task best served by extending a lower library says so rather than working around it silently.
[CLAUDE.md](../../../../CLAUDE.md) states it, under *The libraries are living — surface missing pieces*.
This file is where SGL keeps those findings: capabilities that belong in **clean-core**, which SGL hand-rolls today.

SGL depends on clean-core alone, and must: an editor links it to parse.
So a capability typed-geometry already has is still a gap here, until clean-core has it too.
**Retire an entry in the same change that lands the capability**, and delete the workaround it describes.

---

## clean-core

### A half type: IEEE binary16 with correct rounding

**Wanted:** a 16-bit float in clean-core, converting from `f32` and from `f64` with one rounding to nearest, ties to even, and widening exactly.
typed-geometry's `tg::f16` is that type, so the wish is for it, or its conversion core, one library lower.

**Why:** the interpreter holds `half` values (CHK-346, EVAL-96), and every one of them is rounded from the 32-bit result its family computes.
A literal rounds from its exact `f64` value, since a detour through `f32` would round twice.

**Today:** `half_bits_of` and `float_of_half_bits` in `builtins/impl/soft_math.cc`, marked TEMPORARY.
They round from `f32` and from `f64`, and keep subnormals, infinities and NaNs.
