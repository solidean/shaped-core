#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>

// Capturing return addresses, as distinct from producing a readable stack trace.
//
// cc::stacktrace is std::stacktrace: it bundles capture with symbolization, and it allocates.
// Both are wrong for a recording site and for a crash handler, which want the addresses now and the names later —
// symbolization is orders of magnitude more expensive than the event it would be attached to.
//
// **What a capture costs differs by an order of magnitude across platforms**, and policy should account for it.
// Where a frame pointer is kept (SC_FRAME_POINTERS, so clang and GCC), the walk chases the chain at a few nanoseconds
// a frame.
// Windows has no frame-pointer ABI on x64 and MSVC cannot maintain one, so it unwinds from tables instead — correct,
// and roughly a microsecond for a deep stack.
// So a stack on every error is affordable; a stack on every warning is not.
//
// An address only means something against the module it came from, so a consumer needs the module base table to
// symbolize offline — see libs/base/clean-core/docs/systems/recording-formats.md.

namespace cc
{
struct stack_capture_result;
enum class stack_walk : u8;
} // namespace cc

/// How to get from one frame to the next.
///
/// Two mechanisms with an order of magnitude between them, and no platform offers both.
///
/// Chasing needs a frame pointer that heads a CHAIN, which is a SysV and AAPCS property: Win64 sets its frame pointer
/// to `rsp + offset`, so `/Oy-` there buys a frame pointer with nothing to walk.
/// Naming the choice anyway is what lets `stack_walk_available` report the truth per platform rather than leaving a
/// caller to guess what a capture cost.
enum class cc::stack_walk : cc::u8
{
    /// Chase the chain where this build keeps one, and unwind from tables otherwise.
    automatic = 0,

    /// Chase the frame-pointer chain: a few nanoseconds a frame, and it stops where the chain does — at code built
    /// without one, which on Windows means the moment a walk leaves our own modules.
    frame_pointers,

    /// Unwind from tables: correct through anything with unwind data, and roughly a microsecond for a deep stack.
    /// Unavailable off Windows, where a capture then reports nothing rather than something wrong.
    unwind_tables,
};

/// What a capture managed, beyond the frames themselves.
///
/// The three flags are worth telling apart: a truncated or stopped capture is a correct prefix, and a broken one is a
/// correct prefix of a walk that then hit something it could not trust.
struct cc::stack_capture_result
{
    /// How many entries of `out` were filled.
    isize count = 0;

    /// `out` ran out before the stack did.
    bool truncated = false;

    /// The walk reached `stop_frame` and stopped there deliberately.
    bool stopped = false;

    /// The chain failed validation and the walk gave up early.
    /// Expected for a stack that leaves our frame-pointer-keeping code, and the reason a capture is a best effort
    /// rather than a guarantee.
    bool broken = false;

    /// Whether anything was captured at all.
    [[nodiscard]] explicit operator bool() const { return count > 0; }
};

namespace cc
{
/// Captures the calling thread's return addresses into `out`, innermost first.
///
/// Allocation-free, lock-free and safe from a crash handler, unlike cc::stacktrace.
///
/// `skip` drops that many innermost frames, so a wrapper can leave itself out.
///
/// `stop_frame` is a STACK ADDRESS, not a return address, and the walk stops once it reaches or passes it.
/// A threshold rather than an equality on purpose: the exact frame is often absent — a scope may have been inlined
/// into its caller — and an equality test would silently never match and walk the whole stack believing otherwise.
/// `cc::rec::current_scope_frame()` is what a profiling caller passes, so a capture describes only the part its
/// innermost open scope does not already name.
///
/// Returns an empty capture on a platform with no walkable native stack (wasm), rather than a wrong one.
[[nodiscard]] cc::stack_capture_result capture_stack(cc::span<void*> out,
                                                     isize skip = 0,
                                                     void const* stop_frame = nullptr,
                                                     cc::stack_walk walk = cc::stack_walk::automatic);

/// Captures a stack from a thread's saved machine context rather than from here.
///
/// This is what sampling needs: a thread is suspended, its context read, and its stack walked from outside.
/// `native_context` is a `CONTEXT*` on Windows and null elsewhere, since no other platform walks a foreign thread
/// this way — POSIX sampling runs the walk ON the target, from a signal handler, where plain capture_stack works.
///
/// **The target must not be running.** Walking a live thread's stack reads a chain that is being rewritten underneath.
[[nodiscard]] cc::stack_capture_result capture_stack_from_native_context(void* native_context,
                                                                         cc::span<void*> out,
                                                                         isize skip = 0,
                                                                         void const* stop_frame = nullptr,
                                                                         cc::stack_walk walk = cc::stack_walk::automatic);

/// Whether `walk` is a real option in this build.
/// `automatic` is always available where any walk is.
[[nodiscard]] bool stack_walk_available(cc::stack_walk walk);

/// Whether a foreign thread's context can be walked at all.
/// True only on Windows today, which is the only platform whose sampler runs outside the sampled thread.
[[nodiscard]] bool stack_capture_from_context_available();

/// Whether this build can walk a stack at all.
/// Constant per platform, and false under Emscripten.
[[nodiscard]] bool stack_capture_available();
} // namespace cc
