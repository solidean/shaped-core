# shaped-shader-library coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) and
shaped-graphics' [editorial rules](../../shaped-graphics/docs/coding-guidelines.md) — read those first.
This document only captures what is **slib-specific**, and specifically the rules the code cannot
enforce on itself.

It is intentionally short. **Extend it** whenever you catch yourself breaking something load-bearing
that nothing in the compiler would have stopped.

---

## Only `real_filesystem` touches the disk

Every shader source slib reads goes through a mounted [`slib::filesystem`](../src/shaped-shader-library/filesystem/filesystem.hh).
[`real_filesystem.cc`](../src/shaped-shader-library/filesystem/real_filesystem.cc) and its watch backends
([`impl/watch_backend_*.cc`](../src/shaped-shader-library/filesystem/impl/)) are the **only** files
permitted to include `<filesystem>`, reach `<Windows.h>`, or otherwise touch real storage or the OS.
Nothing else in slib may open a file, stat a path, ask the OS to notify it, or take an absolute path and
act on it.

**Why** (not obvious): three things fall out of that one seam, and all three break if it leaks.

- **Reload tests need no disk and no sleeps.** They mount a `memory_filesystem`; an edit is a `write()`
  and a scan is a `poll_hot_reload()`. A single direct `std::filesystem` call somewhere in the compile
  path would drag real files — and real timing — back into them. This holds for *watching* too:
  `memory_filesystem` fires its watch straight out of `write()`, so the notify path is exercised end to
  end with no OS and no timer. The handful of tests that do use a real directory say so loudly and are
  kept apart.
- **Shipping works without a mode flag.** A shipped binary has no source tree, so its shaders come from
  the embedded copy. That only works because *reading a shader* never means *reading a file*.
- **`..` cannot climb out of a mount.** Path normalization is the traversal guard, and it only guards
  what goes through it.

`<filesystem>` is also not a [blessed clean-core header](../../../base/clean-core/docs/blessed-stdlib-headers.md),
which is the other reason to keep it in one place. When clean-core grows a virtual filesystem, this
library's VFS is the trial run for it and `real_filesystem` — with its backends — is the only thing that
has to move.

The corollary for `watch()`: it is an **optional** capability, and `nullopt` ("I cannot notify — poll me")
must stay a first-class answer rather than a bug. It is what every platform with no backend returns, and
the polling fallback it selects is the only reason a missing backend is a performance note instead of a
broken feature.

## A generated package header is private; publishing one is the caller's job

`sc_add_shader_package` puts the generated `.hh` and `.cc` in the **binary dir** as `PRIVATE` sources of
the declaring target. Do not add them to a `FILE_SET`, do not install them, and do not `#include`
another target's generated header.

If a library wants to expose some of its shaders, it **re-exposes them from its own public header** — a
getter, or a mirrored declaration — and owns the drift between the two:

```cpp
// my-renderer/public_shaders.hh  — hand-written, part of my-renderer's API
namespace my { slib::shader_asset_handle const& vignette_shader(); }
```

**Why** (not obvious): it is what keeps the build graph sound. A custom command's outputs are only an
ordering dependency of the target that *lists* them, so a second target including a generated header it
does not list is a race — it compiles or not depending on build order. One-package-per-target is what
rules that out, and the escape hatch above is what makes the restriction livable.

## Reload stages, it never replaces

The watcher must only ever write an asset's `pending` slot. Promotion happens in `acquire()`, on the
consuming thread, and only once the compile is ready.

**Why** (not obvious): it is what makes all three of these true at once — a consumer never waits on the
compiler, a shader that no longer compiles leaves the running one alone, and a compile still in flight
is invisible rather than a stall. Any shortcut that has the watcher assign `current` directly gives up
all three. The corollary is that the watcher must also *drive* the compile it stages: a `cc::async` node
is cold until someone runs it, and nobody else ever will.
