#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-shader-compiler-msl/fwd.hh>

/// Running Apple's `metal` driver as a child process, which is the only way to obtain a metallib.
///
/// TEMPORARY, and deliberately so: process execution belongs in clean-core next to `platform/environment.hh`, as a
/// `cc::run_process` taking an argv, stdin bytes and giving back stdout bytes, stderr text and an exit code.
/// Nothing in shaped-core has one today — `tools/instruction-tracer`'s `mca_runner.cc` hand-rolled a Windows-only,
/// text-only version for the same reason — so this file is the second hand-rolled copy and should be the last.
/// It is POSIX-only and does exactly what this library needs; replace it with the clean-core API when that exists.
///
/// Known defects, left here on purpose and recorded as requirements in libs/base/clean-core/docs/TODO.md:
///   - SIGPIPE is not ignored, so a child that exits before reading all of stdin kills this process on the next write
///   - `pipe` then `fcntl(FD_CLOEXEC)` races another thread's `posix_spawn`, which can inherit the ends in between
///   - a failed second or third `pipe` leaks the ends already made, and `drain` takes `EINTR` for end-of-stream
///   - without the MetalToolchain component `xcrun -f metal` may still resolve, and then every metallib compile fails

namespace ssc::msl::impl
{
/// What a finished child process left behind.
struct process_result
{
    int exit_code = -1;
    cc::vector<byte> out;
    cc::string err;
};

/// Runs `executable` with `arguments`, writes `input` to its stdin, and collects both of its output streams.
///
/// Reads and writes are interleaved through `poll`, so neither stream deadlocks when a shader is larger than a pipe
/// buffer — the failure that a write-everything-then-read-everything version hits only on big inputs.
/// The error is for a process that could not be started or was killed by a signal; a non-zero exit is a result.
[[nodiscard]] cc::result<process_result> run_process(cc::string_view executable,
                                                     cc::span<cc::string const> arguments,
                                                     cc::string_view input);

/// Asks `xcrun` where the Metal driver is, returning an empty string when there is none.
/// Apple ships the toolchain as a component installed separately from Xcode, so an absent one is ordinary.
[[nodiscard]] cc::string resolve_metal_driver();

/// `metal --version`'s compiler version, e.g. "32023.883"; empty when it would not report one.
[[nodiscard]] cc::string query_driver_version(cc::string_view driver_path);
} // namespace ssc::msl::impl
