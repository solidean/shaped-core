#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>

// Capturing return addresses, as distinct from producing a readable stack trace.
//
// cc::stacktrace is std::stacktrace: it bundles capture with symbolization, and it allocates.
// Both are wrong for a recording site and for a crash handler, which want the addresses now and the names later —
// symbolization is orders of magnitude more expensive than the event it would be attached to.

namespace cc
{
/// Captures the calling thread's return addresses into `out`, innermost first.
///
/// Allocation-free and safe from a crash handler, unlike cc::stacktrace.
/// Returns how many entries of `out` were filled; a full result may mean the stack was deeper than `out`.
///
/// `skip` drops that many innermost frames, so a wrapper can leave itself out.
/// `stop_frame` ends the walk as soon as it is reached, which is how a caller that already has an instrumentation
/// scope below it captures only the part the scope does not already describe.
///
/// **The implementation returns 0 on every platform today**, so a caller gets an empty capture rather than a wrong one.
/// The seam is real, and the fill-in touches this file alone.
[[nodiscard]] isize capture_stack(cc::span<void*> out, isize skip = 0, void const* stop_frame = nullptr);
} // namespace cc
