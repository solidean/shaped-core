# shaped-graphics-language TODO

Running list of short-term follow-ups: what we already know we most likely want, and have not built.
An idea that may be far off, or that we may never want, belongs in the [spec incubator](spec/incubator/_index.md) instead.
What the compiler carries today is the [spec](spec/_index.md); a construct it does not carry yet is `unsupported-yet` there.

- **Hex literals.** `0xFF` is a number, and the checker refuses it as `unsupported-yet: a hex literal`.
  `classify_number` puts every prefixed literal in `number_class::other`, since those need literal types the checker does not have.
  A stencil mask in a `pipeline` is where it bites first.
- **Binary literals.** `0b1010`, the same way and for the same reason: `unsupported-yet: a binary literal`.
