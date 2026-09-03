#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/string/uri.hh>
#include <clean-net/common/error.hh>

/// A URL, checked for the things an HTTP request needs to be true of it.
///
/// **The parsing is `cc::uri`'s**, which is RFC 3986 and knows about percent-escapes, relative references and
/// normalization.
/// What is here is the scheme-specific half that `cc::uri` deliberately leaves out -- its own docs say so: no default
/// port is dropped and no empty path becomes `/`, because both are true of http and false of URIs in general.
///
/// So this type is that knowledge and nothing else: which port to connect to, whether it must be TLS, what goes on
/// the request line, and what a connection pool is keyed on.
struct cnet::http_target
{
    /// The URL as given, owned.
    /// `url.resolve(location)` is how a redirect is followed, which is the reason this keeps the whole thing rather
    /// than just its pieces.
    cc::uri url;

    /// Lower-cased, and without the brackets an IPv6 literal wears -- what the resolver and SNI both want.
    cc::string host;

    /// The port to connect to: the explicit one, or 443 for https and 80 for http.
    i32 port = 0;

    bool secure = false;

    /// Parse and check a URL.
    ///
    /// Fails on a relative reference, on a scheme that is not http or https, on a missing host, and on credentials
    /// in the authority.
    ///
    /// **Credentials are refused rather than dropped.**
    /// `https://evil.com@good.com/` is a URL most readers get the host of wrong, and a client that quietly ignores
    /// the first half is the reason that trick works.
    [[nodiscard]] static cc::result<http_target, error> parse(cc::string_view text);

    /// The same checks over an already-parsed URI, which is how a redirect's `Location` is followed.
    [[nodiscard]] static cc::result<http_target, error> from_uri(cc::uri url);

    /// What goes on the request line: the path and query, `/` when the path is empty, and never the fragment.
    /// A fragment is resolved by the client and a server has no business seeing one.
    [[nodiscard]] cc::string request_target() const;

    /// `scheme://host:port`, with the port left off when it is the scheme's default.
    ///
    /// What a connection pool and a rate limit are keyed on, which is why the default port is dropped: two URLs
    /// reaching the same server must produce the same key.
    [[nodiscard]] cc::string origin() const;

    /// The `Host` header's value: the host, bracketed again if it is an IPv6 literal, and the port when it is not
    /// the scheme's default.
    [[nodiscard]] cc::string host_header() const;
};
