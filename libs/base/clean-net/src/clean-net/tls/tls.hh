#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <clean-net/common/cancel.hh>
#include <clean-net/common/deadline.hh>
#include <clean-net/common/error.hh>
#include <clean-net/transport/tcp.hh>

/// TLS over an established connection.
///
/// **TLS is a wrapper, not a transport.**
/// `cnet::tls_connect` takes a connection and hands back a connection, so it composes with every transport there is:
/// over `native_transport` it is HTTPS, over `virtual_network` it is a handshake with no socket in sight, and over
/// `simulated_transport` it is a handshake on a link that drops records.
///
/// **No TLS library type appears here**, exactly as zstd and libspng are kept private.
/// What backend is underneath is not part of the API, because anything a caller does through a native handle that
/// changes behaviour is behaviour the other backends do not have -- a portability bug shaped like a feature.

/// What a connection is willing to trust.
///
/// The platform's own store is the default and the only correct default: a bundled root set goes stale, ignores the
/// enterprise roots a corporate proxy needs, and ignores the machine owner's own decisions.
struct cnet::tls_trust
{
    /// Trust what this machine trusts.
    bool use_system_roots = true;

    /// Additional roots, in PEM form: a private CA, or a test fixture.
    cc::vector<cc::string> additional_roots_pem;

    /// Accept any certificate, valid or not.
    ///
    /// **Settable from code and from nowhere else** -- never from a URL, an environment variable or a configuration
    /// file, so it cannot be turned on in the field by anything but a recompile.
    /// It exists for a test against a self-signed fixture.
    bool allow_any_certificate = false;
};

/// A certificate chain and the private key that goes with it, both in PEM form.
struct cnet::tls_identity
{
    /// The leaf first, then any intermediates.
    cc::string certificate_chain_pem;

    cc::string private_key_pem;
};

/// What a client hands to a handshake.
struct cnet::tls_options
{
    tls_trust trust;

    /// Application protocols to offer, in preference order -- `h2`, `http/1.1`.
    /// The one the server picked is `cnet::tls_negotiated_alpn`.
    cc::vector<cc::string> alpn;

    /// A client certificate, for a server that asks for one.
    cc::optional<tls_identity> client_identity;
};

/// What a server hands to a handshake.
struct cnet::tls_server_options
{
    /// What the server presents.
    /// A server without one cannot handshake at all, which is why this is not optional.
    tls_identity identity;

    /// Application protocols the server is willing to speak, in preference order.
    cc::vector<cc::string> alpn;
};

namespace cnet
{
/// Whether this build has a TLS backend at all.
///
/// False on wasm, where the browser holds the TLS and a program never sees a handshake.
[[nodiscard]] bool tls_is_supported();

/// Wrap an established connection in TLS, as the client.
///
/// `hostname` is what the certificate must match and what is sent as SNI, so it is the NAME the caller connected to
/// rather than the address it resolved to -- passing an address here is how certificate validation is accidentally
/// disabled.
///
/// The returned connection owns the one it was given: reads and writes go through the record layer, and closing it
/// closes both.
/// On failure the underlying connection is closed rather than handed back, because a half-negotiated stream is not a
/// stream anybody can use.
///
/// Fails with `tls_handshake_failed` when the handshake broke down, and with `certificate_rejected` when a chain was
/// built and refused -- expired, wrong host, unknown issuer.
/// The two are separate because only the second is a decision.
[[nodiscard]] cc::shared_async<cc::shared_ptr<tcp_connection>> tls_connect(cc::shared_ptr<tcp_connection> connection,
                                                                           cc::string_view hostname,
                                                                           tls_options const& options = {},
                                                                           deadline d = deadline::after_secs(30),
                                                                           cancel_token const& token = {});

/// Wrap an accepted connection in TLS, as the server.
/// For the loopback dev server, and for a test that wants both ends of a real handshake.
[[nodiscard]] cc::shared_async<cc::shared_ptr<tcp_connection>> tls_accept(cc::shared_ptr<tcp_connection> connection,
                                                                          tls_server_options const& options,
                                                                          deadline d = deadline::after_secs(30),
                                                                          cancel_token const& token = {});

/// Make a self-signed identity for `hostname`, generated here and now.
///
/// For a loopback dev server, and for a test: a certificate nobody else trusts, generated in-process, is the honest
/// shape for a server that only ever answers to this machine.
/// It is a P-256 key and a SHA-256 signature, which takes about a millisecond.
///
/// **The validity window is fixed rather than relative** -- 2020 through 2035 -- because a certificate minted for
/// "the next hour" is a test that fails on a machine whose clock is off, and this is not a certificate anybody
/// should be relying on for freshness.
/// It is marked as its own CA so it can be handed to a client as a root, which is what a private CA looks like.
[[nodiscard]] cc::result<tls_identity, error> tls_make_self_signed(cc::string_view hostname);

/// The application protocol both ends agreed on, or empty when none was offered or none matched.
/// Empty for a connection that is not a TLS one at all.
[[nodiscard]] cc::string_view tls_negotiated_alpn(tcp_connection const& connection);
} // namespace cnet
