---
created: 2026-11-07
refs:
  - file: tools/shaped-linter/rules/prose/no-stock-words/no_stock_words.cc
---

# A prose rule for American spelling: color, meters, center

Prose is written in American English: `color`, not `colour`, and `meters`, not `metres`.
CLAUDE.md states it; nothing checks it yet, and a convention nothing checks drifts.

Wanted: a rule beside `no-stock-words`, a list of British spellings each with its American one, firing in every place prose rules look.
Unlike `no-stock-words` it can carry a fix, since each entry has one replacement.

What is known, counted on 2026-11-07 with `git grep` over this repository, case folded:

| Spelling | Count |
|---|---|
| cancelled | 300 |
| behaviour | 152 |
| centre | 141 |
| colour | 136 |
| neighbour, neighbours, neighbouring | 191 |
| grey | 75 |
| labelled | 26 |
| honour | 26 |
| travelled | 20 |
| modelled | 17 |

Points to settle before the list is final:

- `cancelled` is common in American use too, and it is an identifier here; whether it is on the list, and whether identifiers follow, is a decision.
- The rule binds prose only; identifiers in code, such as a `colour` member, change with an API decision of their own.
- `-ise` verbs (`normalise`, `initialise`) are British but also appear in quoted external names; a match inside backticks is left alone.
- Hyphenation and quoted text: a word inside a code span or a link target is never touched.

The fix pass across the existing prose is a separate step, run once the rule exists, so each spelling's change is one reviewable commit.
