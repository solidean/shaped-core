#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-net/fwd.hh>

/// Why a networking call failed.
///
/// Three kinds of "no" live here and they are not interchangeable.
/// `unsupported` is a property of the platform and will never change, so a caller decides once at startup.
/// `backend_missing` is a property of the build, so it changes when someone fetches a dependency.
/// Everything else happened this time and may not happen next time.
///
/// **Absence is never faked.** Nothing in this library returns an empty body, a zero byte count or a null endpoint
/// where it means "could not".
enum class cnet::error_code : cnet::u8
{
    /// A failure this enum does not name; `native_code` carries the platform's own number.
    unknown,

    /// This platform has no such concept and never will -- a listening socket in a browser, a datagram over `fetch`.
    /// Answered once, at startup.
    unsupported,

    /// The capability exists on this platform but was not compiled into this build.
    /// A fetched dependency that was never fetched is the usual cause.
    backend_missing,

    /// The call was wrong rather than the world: a malformed URL, a port out of range, a header with a newline in it.
    invalid_argument,

    /// The operation needed a feature level the chosen backend does not reach.
    /// A programming error against a known backend, and a portability error against `automatic`.
    level_not_supported,

    /// The deadline passed before the operation finished.
    timed_out,

    /// The caller cancelled it.
    cancelled,

    // ---- connecting ----

    /// The name has no address, or the resolver could not answer.
    name_not_resolved,

    /// Something answered and said no.
    connection_refused,

    /// There is no route to that host or network from here.
    host_unreachable,

    /// The peer or an intermediary dropped an established connection.
    connection_reset,

    /// The peer closed cleanly, mid-operation.
    /// Distinct from `connection_reset`: nothing went wrong, the bytes simply stopped.
    connection_closed,

    /// The address is already in use, which for a listener usually means another process has the port.
    address_in_use,

    /// The OS refused: a privileged port, a sandbox, a missing capability.
    permission_denied,

    // ---- TLS ----

    /// The handshake failed before any trust decision was reached.
    tls_handshake_failed,

    /// The chain was built and rejected -- expired, wrong host, unknown issuer.
    /// Separate from the handshake failure because only this one is a decision rather than a breakdown.
    certificate_rejected,

    // ---- protocol ----

    /// The peer's bytes do not parse as the protocol it claimed to speak.
    protocol_error,

    /// The response body exceeded what the caller allowed, and was refused rather than buffered.
    body_too_large,

    /// The redirect chain was longer than the caller allowed.
    too_many_redirects,
};

/// What a failed clean-net call reports.
///
/// Copyable on purpose, unlike cc::any_error, so a caller can latch the first failure and still read it afterwards.
/// It converts into cc::result<T, cc::any_error> implicitly, so a caller that does not care about the code loses
/// nothing.
struct cnet::error
{
    error_code code = error_code::unknown;

    /// The platform's own number -- `errno`, `WSAGetLastError`, a TLS library's code.
    /// 0 when no platform call was made, which is the case for every `unsupported` and `backend_missing`.
    i32 native_code = 0;

    cc::string message;

    /// The ADL hook cc::to_debug_string finds, which is what makes the erasure into cc::any_error carry the message.
    [[nodiscard]] friend cc::string to_string(error const& e) { return e.message; }
};

namespace cnet
{
/// The spelling of `code` as it appears in this enum, for a log line.
/// Never parse it; branch on the code.
[[nodiscard]] cc::string_view to_string(error_code code);

/// The error every entry point reports where the platform has no such concept.
[[nodiscard]] error unsupported_here(cc::string_view what);

/// The error every entry point reports where the backend exists but was not compiled in.
[[nodiscard]] error backend_missing(cc::string_view what, cc::string_view how_to_get_it);
} // namespace cnet
