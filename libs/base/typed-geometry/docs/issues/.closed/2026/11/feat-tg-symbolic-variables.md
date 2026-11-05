---
created: 2026-11-03
priority: next
parent: epic-tg-polynomials
closed: 2026-11-03
resolution: fixed
---

# tg::dynamic_vars: multivariate polynomials over interned symbols, for computer algebra

Wanted:
- `tg::symbol`: a process-wide interned name, cheap to compare.
- `tg::dynamic_vars<Order>`: each polynomial carries its sorted variable list and a runtime exponent width that grows when needed.
- Operands over different lists merge them and repack one side; the same list is a pointer compare.
- Printing with the variables' names, and parsing, for tests and interactive use.

Done in `symbolic/symbol.hh` and `symbolic/dynamic_polynomial.hh`, with one change from the above:
the exponent width stays a fixed, checked 16 bits rather than growing, as `vars<>`'s does.
Tests hold it against the static type term for term under the same order, and over nine variables, three words a term, against evaluation.
