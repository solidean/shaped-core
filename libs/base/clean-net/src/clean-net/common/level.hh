#pragma once

#include <clean-core/string/string_view.hh>
#include <clean-net/fwd.hh>

/// How much of HTTP a backend can actually do.
///
/// A ladder rather than a bag of capability queries, for the property a bag cannot give: code written against a level
/// runs on every backend at that level or above.
/// A caller checks once, not per feature, and a new backend cannot quietly add a query that existing call sites never
/// make.
///
/// The ordering is real rather than a convenience.
/// Everything at `client` is expressible over any complete HTTP stack, including the platform ones on iOS and
/// Android -- which is what makes it the portable target for anything serious.
/// `connection` exists precisely because taking the socket is *not* expressible over a platform stack, so folding it
/// into `client` would put `client` out of reach on the platforms it is there to serve.
///
/// **The transport layer is not on this ladder.** Sockets, listeners and datagrams are a different API rather than
/// more of this one, so they answer `is_supported()` instead of a level.
enum class cnet::http_level : cnet::u8
{
    /// What a browser's `fetch` can do: a method, a URL, a restricted header set, and a body.
    /// Redirects are followed for you and cannot be inspected, connections are not yours to see, and the same-origin
    /// policy applies to requests a server may still refuse.
    fetch = 0,

    /// A complete HTTP client: every header, explicit redirect control, connection reuse and pooling, a streamed
    /// request body, and response trailers.
    ///
    /// One budget covers a whole request rather than each phase of it, deliberately: a per-phase timeout lets a
    /// request to a four-address host take four times what the caller asked for.
    client = 1,

    /// The connection itself: protocol upgrades to something we did not write, client certificates, and per-socket
    /// options.
    connection = 2,
};

namespace cnet
{
/// The spelling of `level`, for a log line and for the message on a `level_not_supported` failure.
[[nodiscard]] cc::string_view to_string(http_level level);
} // namespace cnet
