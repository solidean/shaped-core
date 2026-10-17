---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/tests/corpus
---

# A linter for SGL's own style, starting with `@expect` on its own line

An `@expect(…)` stands on the line above its `test`, never before it on the same line; every file here follows that.
It is a rule of `@expect` and not of attributes: `@vertex fun main(…)` on one line reads fine and stays.
The parser takes both spellings, so only a linter can hold the line.
