---
created: 2026-11-03
priority: next
parent: epic-tg-polynomials
---

# tg::dynamic_vars: multivariate polynomials over interned symbols, for computer algebra

Wanted:
- `tg::symbol`: a process-wide interned name, cheap to compare.
- `tg::dynamic_vars<Order>`: each polynomial carries its sorted variable list and a runtime exponent width that grows when needed.
- Operands over different lists merge them and repack one side; the same list is a pointer compare.
- Printing with the variables' names, and parsing, for tests and interactive use.
