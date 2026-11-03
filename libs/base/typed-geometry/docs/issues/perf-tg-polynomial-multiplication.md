---
created: 2026-11-03
priority: next
parent: epic-tg-polynomials
---

# Polynomial multiplication at FLINT's speed or better

To measure, against FLINT's `fmpz_mpoly` and `fmpz_poly`:
- A fused `bigint::assign_add_mul`, so accumulating a product's coefficient makes no temporary.
- Kronecker substitution into a dense accumulator when the product's packed exponents span a small range, which infinitesimal polynomials typically do.
- Monagan and Pearce's heap multiplication otherwise.
- Workloads: predicates under symbolic perturbation, Fateman's (1 + x + y + z + t)^20 squared, a sparse product, univariate products and division.

Progress, in instructions retired on a Zen 5 (libs/base/typed-geometry/docs/modules/symbolic.md has the table):
- `bigint::assign_add_mul` and `assign_sub_mul`: one-limb factors into an accumulator of up to two limbs inline, the rest through a thread-local product.
- Dense products by Kronecker substitution, heap products otherwise, and in-place merges for rvalue sums.
- Fateman's n = 10 went from 568 M to 104 M, the sparse n = 6 from 286 M to 152 M, orient3d under perturbation from 106,000 to 70,000.

Left:
- The FLINT comparison itself.
- The infinitesimal case is dominated by allocation: a node per non-zero bigint coefficient, two vectors per polynomial.
  FLINT's fmpz keeps values below 2^62 inline, which tg::bigint does not (hypothesis: the larger part of the gap, unmeasured).
- Univariate products of multi-limb coefficients take the scratch path; FLINT packs them by Kronecker substitution into one integer product.

Against FLINT 3.3.1, built with MSVC without its assembly, GMP being CGAL's generic build; instructions retired, tg / FLINT:
| product | terms | tg | FLINT |
|---|---|---|---|
| orient3d over four infinitesimally perturbed points | 64 | 83,000 | 99,000 |
| Fateman, n = 10 | 10,626 | 24.6 M | 12.6 M |
| sparse five variables, n = 6 | 114,000 | 94.5 M | 62 M |
| univariate degree 30, two-limb coefficients | 61 | 91,000 | 138,000 |
| univariate degree 100 | 201 | 734,000 | 341,000 |

What got there:
- `tg::bigint_accumulator<W>` and `bigint_word_accumulator`: two's-complement sums of products for factors of one or two limbs, or i64.
  The products pick them through duck typing (`C::template accumulator<W>`, `C::word_accumulator`) when every coefficient fits.
- Factors read once into word form, so the inner loops stay off the bigints' nodes; loop invariants in locals against aliasing.
- A dense box no larger than the pairs is accumulated untracked and walked once; a sparser one records touched slots.

Left, in the order they pay:
- The sparse product: 1.5 times FLINT's instructions, where the rest goes not measured yet.
  It was bound by node allocation at 147 M; clean-core's bounded slab walk took it to 94.5 M, and its cycles from 185 M to 41 M.
- Fateman's box, 4.6 MB of accumulators at n = 10, leaves L2: chunking by the leading variable's degree, as FLINT's array method does.
- Univariate products past degree 100: Kronecker substitution into one integer product, which wants a subquadratic bigint product.
