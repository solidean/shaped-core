---
created: 2026-10-17
tags: [dx12, flaky]
refs:
  - test: shaped-graphics-test
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_expected_messages.hh
---

# A barrier-only dx12 submit once landed outside any test

One full `dev.py check` failed `shaped-graphics-test` on debug-nopch with the debug layer's "recorded only Barrier commands" warning, logged under no test owner.
The message is allowlisted, but only inside a test, so an unowned one fails the run.
It did not reproduce in 105 further runs of the suite on that preset, 60 of them under concurrent load.

Ruled out by reading: the copy-queue windows record only copies, `prepare_texture_for_async` and routine ticks run on the test's thread, and shutdown submits nothing.
A barrier-only list is almost always an entry pre-list, whose need depends on submit order, which fits a rare failure.
Which thread submitted without an owner is unknown.

**Next step when it recurs:** have the dx12 relay name the raising thread, and attach a stack to this one message.
