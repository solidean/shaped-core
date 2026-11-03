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
