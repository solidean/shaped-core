---
created: 2026-10-31
---

# The language server marks a dot call that changes its receiver

A dot call may bind a `mut` first parameter to a place, so `ui.print_txt(…)` changes `ui` with no `mut` written at the call.
The mark a free call carries is then gone from the text, and the editor is where it comes back.

- **Wanted**: the language server underlines, or otherwise decorates, the method name of a dot call whose receiver binds a `mut` parameter, as the "this call changes its receiver" affordance.
- A semantic token modifier is the likely carrier, so every LSP client shows it without client code.
