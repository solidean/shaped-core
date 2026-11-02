---
created: 2026-10-27
refs:
  - file: libs/base/clean-core/tests/memory/allocation-test.cc
  - test: allocation - a failed allocate_bytes calls the installed handler, which may throw
---

# allocation-test's refused huge allocation aborts under ASan

`allocation - a failed allocate_bytes calls the installed handler, which may throw` requests `1 << 60` bytes from `cc::system_memory_resource`, expecting the system allocator to refuse.
Under ASan `posix_memalign` does not return null: it reports `allocation-size-too-big` and aborts the whole `clean-core-test`, exit code -6.
That preset is `sanitize-macos-arm-llvm`, which `dev.py check` runs on a Mac.
So `check` fails on macOS for every change since `6581e1938` added the test.

The fix is either `allocator_may_return_null=1` in the sanitizer runs' `ASAN_OPTIONS`, or skipping the system-resource half of the test where ASan is on.
