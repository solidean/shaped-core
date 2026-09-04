#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <clean-net/address/ip_address.hh>
#include <clean-net/common/error.hh>

/// The platform's own name resolution, and the only place `getaddrinfo` is named.
///
/// **This blocks, with no timeout and no cancellation**, which is the whole reason the resolver above it owns a
/// thread.
/// There is no portable asynchronous form: `getaddrinfo_a` is glibc-only and implemented with threads anyway, and
/// every platform that has something better has a different something.

namespace cnet::impl
{
/// Whether this build can turn a name into an address at all.
/// False on wasm, where the browser resolves inside `fetch` and a hostname never becomes an address here.
[[nodiscard]] bool resolve_is_supported();

/// Ask the OS for every address of `host`, in the order it offers them.
///
/// Both families come back; picking between them belongs above, where happy eyeballs can race them.
/// The OS order is kept because it already reflects RFC 6724 sorting, which knows things about this machine's routes
/// that we do not.
[[nodiscard]] cc::result<cc::vector<ip_address>, error> resolve_blocking(cc::string_view host);
} // namespace cnet::impl
