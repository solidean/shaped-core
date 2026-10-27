---
created: 2026-10-17
refs:
  - file: tools/instruction-tracer/src/instruction-tracer/debug/mca_runner.cc
  - file: libs/graphics/shaped-shader-compiler-msl/src/shaped-shader-compiler-msl/impl/metal_driver.hh
  - file: libs/graphics/shaped-shader-compiler-msl/src/shaped-shader-compiler-msl/impl/metal_driver.cc
---

# A subprocess API: an argv, stdin bytes in, stdout bytes, stderr text and an exit code out

Two hand-rolled copies exist and both wait for it: `tools/instruction-tracer`'s `mca_runner.cc`, Windows-only and text-only, and ssc::msl's `impl/metal_driver.cc`, POSIX-only.

The POSIX copy's known defects are the requirements, since each is easy to get wrong again:

- **SIGPIPE** (plausible, not reproduced): nothing in the tree ignores it.
  A child that exits before reading all of stdin makes the next `write` kill the process rather than return `EPIPE`.
  On macOS the fix is `F_SETNOSIGPIPE` on the write end.
- **Close-on-exec race**: `pipe` followed by `fcntl(FD_CLOEXEC)` leaves a window in which another thread's `posix_spawn` inherits the new ends.
  That is the very hang the flag exists to prevent, where a pipe never reaches end-of-file.
  The fix is `POSIX_SPAWN_CLOEXEC_DEFAULT` on a `posix_spawnattr_t`, or `pipe2(O_CLOEXEC)` where it exists.
- **Leaks and EINTR**: a failed second or third `pipe` leaks the ends already made, and the reader takes `EINTR` for end-of-stream.
- **Toolchain detection** (plausible, ssc::msl's own): without Apple's MetalToolchain component, `xcrun -f metal` may still print a path while running `metal` fails.
  Then `automatic` picks the metallib arm and every compile fails.
  Check on a Mac without the component whether `is_available` should also require `query_driver_version` to return a version.
