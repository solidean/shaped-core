---
created: 2026-08-26
blocked-by: [feat-review-symbol-links]
refs:
  - file: tools/review/assets/file.js
---

# A semantic peek: the enclosing declaration with its doc comment

Hovering a file reference shows a window: up through the comment block above the line, down by the review's context setting.
That rule is mechanical and good enough to be worth having.

**What a reader actually wants is the enclosing declaration with its doc comment**, which is the symbol index under a different name.
So whoever builds the index for [feat-review-symbol-links](feat-review-symbol-links.md) has a second customer here.
