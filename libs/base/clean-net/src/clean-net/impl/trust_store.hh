#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
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
/// Every root this machine trusts, DER-encoded.
///
/// DER rather than PEM because that is what the platform APIs hand over, and re-encoding to text only to parse it
/// back would be two conversions in service of nothing.
///
/// Fails with `unsupported` where the adapter for this platform is not written yet, which is a real answer rather
/// than an empty list: no roots and "I could not ask" are the same set and very different facts, and only the first
/// should let a caller believe a connection was verified.
[[nodiscard]] cc::result<cc::vector<cc::vector<byte>>, error> system_root_certificates();
} // namespace cnet::impl
