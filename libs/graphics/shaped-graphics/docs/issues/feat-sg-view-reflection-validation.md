---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/views.md
---

# Validate a view's `T` and view class against shader reflection

`T` on a typed view is a host-side safety and stride handle, not a wire format.
Reflection reports a binding's layout and kind, never a C++ type, so `T` is *validated* against it rather than replaced by it.
That means `sizeof(T)` against the reflected stride, and the view kind against the reflected kind.
Because the call site already fixed `T`, much of that check is compile-time.
See [concepts/views.md](../concepts/views.md).
