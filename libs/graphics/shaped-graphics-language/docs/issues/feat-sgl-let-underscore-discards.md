---
created: 2026-11-22
priority: next
tags: [porting-feedback]
---

# let _ = discards a value

`let _ = bump(mut x)` is `unsupported-yet: a pattern in let`.
A bare `bump(mut x)` statement compiles already, so this blocks nothing; it is the spelling a port reaches for, and the one that says the result is dropped on purpose.

Wanted: `_` as the one pattern `let` takes, evaluating its value and binding nothing.
Other patterns stay `unsupported-yet`.
