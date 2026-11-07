---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/_index.md
---

# Are all of the spec's rules needed, or can they be merged

What is wrong or wanted, and what is already known.

The spec holds 1,290 numbered rules, and `EMIT` alone reached number 241.
That may be what a language this size needs, or it may be rules that grew one per change and now overlap.
Either answer is worth having, so this is a review with a written result rather than a cleanup with a target number.

## Counted on 2026-11-07

Rule definitions per label, across `docs/spec/`:

| label | rules | label | rules |
|---|---|---|---|
| `CHK` | 456 | `GRP` | 37 |
| `EMIT` | 213 | `TOK` | 35 |
| `AST` | 162 | `OP` | 33 |
| `EVAL` | 125 | `LINE` | 26 |
| `LEGAL` | 70 | `NUM` | 19 |
| `STR` | 46 | `SYN` | 9 |
| `FORM` | 43 | `DIAG` | 9 |
|  |  | `CMT` | 7 |

- `EMIT` has 213 definitions and reached 241, so numbers are not contiguous; why has not been looked at.
- [emitting.md](../spec/semantics/emitting.md) is 805 lines, which sits at the edge of the repo's file-size guideline.
- 94 files under `src/` and `tests/` cite an `EMIT` number, so a merge is not free: each citation must move with it.

## Questions

- **Is a rule one testable claim?** A rule that holds several may want splitting, and rules stating one claim twice want merging.
- **Do the late `EMIT` rules overlap?** EMIT-235 to EMIT-241 each state what a native route does, and several restate a text-route rule with a different target.
  A family may read better as one rule and a table.
- **Is a rule what the numbering is for?** A number earns its place by being cited from a test or from code; a rule nothing cites may be prose.
  This is hypothesis: nobody has measured how many rules are cited.
- **Does `CHK`'s size reflect the language or the file layout?** It is more than a third of the total.

## What to produce

1. A count of cited and uncited rules per label, and of rules that no test exercises.
2. A list of merges worth doing, with the citations each one moves.
3. A short note in the spec's own docs on what a rule is and when to add one, whichever way the review falls.
   If the count is legitimate, that note says so and why, so the next reader need not ask.

Renumbering is not a goal: a retired number stays retired, since numbers are cited from history.
