---
created: 2026-10-17
blocked-by: [feat-cc-subprocess-api]
refs:
  - file: libs/graphics/shaped-shader-compiler-msl/src/shaped-shader-compiler-msl/impl/metal_driver.hh
  - file: libs/graphics/shaped-shader-compiler-msl/src/shaped-shader-compiler-msl/impl/metal_driver.cc
  - file: libs/graphics/shaped-shader-compiler-msl/.shaped-lint.yml
  - symbol: ssc::msl::impl::run_process
---

# ssc::msl hand-rolls process execution, which belongs in clean-core

Running Apple's `metal` driver as a child process is the only way to obtain a metallib, and clean-core has no process API.
So `impl/metal_driver.cc` spawns it with POSIX calls, under a lint exemption for their headers.
`tools/instruction-tracer`'s `mca_runner.cc` hand-rolled a Windows-only, text-only version for the same reason, so this is the second copy and should be the last.

What replaces it: a `cc::run_process` next to `platform/environment.hh`, taking an argv and stdin bytes and giving back stdout bytes, stderr text and an exit code.

Known defects of this copy, left on purpose until then:

- SIGPIPE is not ignored, so a child that exits before reading all of stdin kills this process on the next write.
- `pipe` then `fcntl(FD_CLOEXEC)` races another thread's `posix_spawn`, which can inherit the ends in between.
- A failed second or third `pipe` leaks the ends already made, and `drain` takes `EINTR` for end-of-stream.
- Without the MetalToolchain component `xcrun -f metal` may still resolve, and then every metallib compile fails.
