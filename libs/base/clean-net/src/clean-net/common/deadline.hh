#pragma once

#include <clean-net/fwd.hh>

/// How long an operation may take before it fails with `error_code::timed_out`.
///
/// Relative rather than absolute, because that is what a call site can write honestly: the operation knows when it
/// started and the caller does not.
/// One deadline covers a whole operation rather than each step of it, so a request that resolves, connects,
/// handshakes and then reads spends one budget instead of four -- which is the difference between a bounded request
/// and one that can take four times as long as anything the caller asked for.
///
/// **Every operation that touches the network has a finite default.**
/// "No timeout" as a default is how a half-open connection becomes a hung program, and the number of callers who
/// remember to pass one for a convenience download is zero.
struct cnet::deadline
{
    /// Milliseconds from when the operation starts; negative means no deadline at all.
    i64 timeout_ms = -1;

    /// No deadline.
    /// Spelled out at the call site, because it is never what you want by accident.
    [[nodiscard]] static deadline never() { return {.timeout_ms = -1}; }

    [[nodiscard]] static deadline after_ms(i64 ms) { return {.timeout_ms = ms}; }
    [[nodiscard]] static deadline after_secs(f64 secs) { return {.timeout_ms = i64(secs * 1000.0)}; }

    [[nodiscard]] bool is_finite() const { return timeout_ms >= 0; }

    [[nodiscard]] friend bool operator==(deadline const&, deadline const&) = default;
};
