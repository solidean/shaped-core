#include "native_resolve.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-net/impl/native_socket.hh>

// CNET_HAS_SOCKETS is a PRIVATE define set by clean-net's CMakeLists, and it gates this file exactly as it gates the
// socket layer: wasm has no resolver of its own, because the browser resolves inside fetch.
#ifndef CNET_HAS_SOCKETS
#define CNET_HAS_SOCKETS 0
#endif

#if CNET_HAS_SOCKETS

#if defined(_WIN32)
// clang-format off
// winsock2 must precede ws2tcpip, whose declarations -- getaddrinfo among them -- are written in terms of it.
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace cnet::impl
{
namespace
{
[[nodiscard]] cc::optional<ip_address> address_of(addrinfo const& info)
{
    if (info.ai_family == AF_INET && info.ai_addr != nullptr)
    {
        auto const& v4 = *reinterpret_cast<sockaddr_in const*>(info.ai_addr);
        auto const* const src = reinterpret_cast<u8 const*>(&v4.sin_addr);
        return ip_address::from_v4(cc::span<u8 const>(src, 4));
    }

    if (info.ai_family == AF_INET6 && info.ai_addr != nullptr)
    {
        auto const& v6 = *reinterpret_cast<sockaddr_in6 const*>(info.ai_addr);
        auto const* const src = reinterpret_cast<u8 const*>(&v6.sin6_addr);
        return ip_address::from_v6(cc::span<u8 const>(src, 16), u32(v6.sin6_scope_id));
    }

    return {};
}
} // namespace

bool resolve_is_supported()
{
    return true;
}

cc::result<cc::vector<ip_address>, error> resolve_blocking(cc::string_view host)
{
    if (host.empty())
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::string("resolve: the host is empty")});

    ensure_socket_platform();

    // SOCK_STREAM rather than nothing at all: without it the OS reports one entry per socket type, so every address
    // would arrive three times.
    auto hints = addrinfo();
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    auto name = cc::string(host);
    addrinfo* results = nullptr;

    auto const failure = ::getaddrinfo(name.c_str_materialize(), nullptr, &hints, &results);
    if (failure != 0)
    {
        // getaddrinfo reports EAI_* rather than errno, so this code belongs to a different space than every other
        // native_code in this library -- which is why the message says what it was.
        return cc::error(error{.code = error_code::name_not_resolved,
                               .native_code = i32(failure),
                               .message = cc::format("could not resolve {}", host)});
    }

    auto addresses = cc::vector<ip_address>();
    for (auto const* it = results; it != nullptr; it = it->ai_next)
    {
        auto const address = address_of(*it);
        if (!address.has_value())
            continue;

        // The same address can arrive more than once across the OS's entries, and a caller racing duplicates would
        // race the same machine twice.
        auto duplicate = false;
        for (auto const& seen : addresses)
            if (seen == address.value())
            {
                duplicate = true;
                break;
            }
        if (!duplicate)
            addresses.push_back(address.value());
    }

    ::freeaddrinfo(results);

    if (addresses.empty())
        return cc::error(error{.code = error_code::name_not_resolved,
                               .native_code = 0,
                               .message = cc::format("{} resolved to no usable address", host)});

    return addresses;
}
} // namespace cnet::impl

#else // CNET_HAS_SOCKETS -- the browser resolves inside fetch, so a name never becomes an address here

namespace cnet::impl
{
bool resolve_is_supported()
{
    return false;
}

cc::result<cc::vector<ip_address>, error> resolve_blocking(cc::string_view)
{
    return cc::error(unsupported_here("name resolution"));
}
} // namespace cnet::impl

#endif // CNET_HAS_SOCKETS
