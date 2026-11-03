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
