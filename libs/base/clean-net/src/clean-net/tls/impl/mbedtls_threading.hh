#pragma once

/// The one thing every user of Mbed TLS in this library must do first.

namespace cnet::impl
{
/// Install the mutex operations Mbed TLS is configured to call, exactly once.
///
/// Its PSA layer keeps process-wide state that every TLS 1.3 handshake goes through, so a program handshaking on two
/// threads at once needs these -- without them the damage shows up as an occasional handshake failing for no visible
/// reason, which is the worst way for a bug like this to present.
///
/// **Every entry point into Mbed TLS calls this first**, the WebSocket handshake's hashing included: the mutex
/// operations are null until it runs, and the first thing to touch a context with null operations fails rather than
/// races.
void ensure_mbedtls_threading();
} // namespace cnet::impl
