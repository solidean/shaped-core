---
created: 2026-10-17
priority: someday
refs:
  - file: libs/base/nexus/src/nexus/async-test.hh
  - file: libs/base/nexus/docs/parallel-execution.md
---

# Can an async invocation admit an `exclusive()` child under a non-exclusive driver?

Today it is refused: the driver holds the phase lock shared, so a child asking for the whole phase would wait on its own driver.

Tags are already arranged: a child's `exclusive(tag)` is taken from the phase around it, refused only when driver and child both hold tags.
The expectation was that `exclusive()` works the same way.

The shape to try: the invocation releases or upgrades the driver's shared hold around an exclusive child.

Nothing in the tree needs it yet; the case to wait for is an invocable that genuinely cannot share its phase with its siblings.
