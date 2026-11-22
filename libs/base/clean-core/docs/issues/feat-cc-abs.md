---
created: 2026-11-22
---

# clean-core has no `cc::abs`

The absolute value of an integer or a float exists only as `tg::abs`, so code that links clean-core alone has none.
The first project started on shaped-core reached for `cc::abs` and found nothing.

Wanted: `cc::abs` for the integer and floating-point primitives, with `tg::abs` then taking it for scalars, or a decision that `tg` is its home.
An integer's minimum has no absolute value, which the function must state.
