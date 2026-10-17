---
created: 2026-10-17
tags: [upstream]
refs:
  - file: libs/graphics/shaped-shader-compiler-dxc/src/shaped-shader-compiler-dxc/compiler.cc
  - symbol: pin_utf8_locale
---

# A process-lifetime locale reference works around DXC's ScopedLocale

Off Windows, DXC's ScopedLocale (its WinAdapter.h) builds and destroys a `locale_t` around every string conversion it makes.
The locale data behind it is one refcounted process-global in glibc, loaded and freed under a glibc-internal rwlock, so concurrent compiles queue on locale bookkeeping.
`pin_utf8_locale` holds one reference forever, which keeps the data loaded and also stops ThreadSanitizer reporting the cross-thread load / free pairs as races.

The fix belongs upstream: ScopedLocale should cache a locale rather than rebuild one per call.
Once a DXC with that fix is pinned, the pin goes.
