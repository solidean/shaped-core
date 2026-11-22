---
created: 2026-11-22
priority: next
tags: [porting-feedback]
---

# A let binds a texture or a buffer, as a call's parameter does

`let src = io.pts` is `unsupported-yet: a buffer as a value; read an element of it, as in values[i], or hand it to a call`, and a texture is refused alike.
A port then indexes the binding again at every use.

Wanted: a `let` of a resource is what a parameter of one is already, a name for a resource the compile resolves.

- **The one requirement is that inlining and hoisting undo it completely**: every use reaches one binding member, or one element of a binding array, at compile time.
- Where they cannot, such as a `let` chosen by a branch on a runtime value, it is an error naming why, never a target's own resource variable.
- A binding array's element by a runtime index is one such resolved element, the index evaluated once.
