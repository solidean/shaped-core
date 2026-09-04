#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-net/common/error.hh>

/// The machine's own root certificates, and the only place a platform trust API is named.
///
/// **This is the hard half of TLS, not the handshake.**
/// Several good libraries do key exchange, ciphers and framing; none of them can tell you what this machine trusts,
/// because every operating system keeps that set behind its own API and none of them is a file a portable library can
/// read.
///
/// **No bundled root set, ever.**
/// One goes stale, ignores the enterprise roots a corporate proxy needs, and ignores the decisions the machine's
/// owner has already made -- so the answer is always this machine's store or nothing.

namespace cnet::impl
{
/// The roots, in whichever form the platform keeps them.
///
/// Two forms rather than one because that is how they arrive: Windows and Apple hand over parsed certificates as
/// DER, while a Linux trust store is a file of concatenated PEM.
/// Converting either way would be an encode and a decode in service of nothing -- the parser takes both.
struct system_roots
{
    cc::vector<cc::vector<byte>> der;

    /// Whole bundles, verbatim: a PEM file holding many certificates parses as one blob.
    cc::vector<cc::string> pem;

    [[nodiscard]] bool empty() const { return der.empty() && pem.empty(); }
};

/// Every root this machine trusts.
///
/// Fails with `unsupported` where the adapter for this platform is not written yet, which is a real answer rather
/// than an empty set: no roots and "I could not ask" are the same list and very different facts, and only the first
/// should let a caller believe a connection was verified.
[[nodiscard]] cc::result<system_roots, error> system_root_certificates();
} // namespace cnet::impl
