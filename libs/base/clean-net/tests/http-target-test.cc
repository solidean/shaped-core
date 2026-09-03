#include <clean-net/http/http_target.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// What an HTTP request needs to know about a URL, over cc::uri's RFC 3986 parsing.
// The interesting cases are the ones a parser is supposed to REFUSE, since every one of them is a way for a request
// to go somewhere other than where it reads as going.

TEST("cnet - an http URL yields what a request is built from")
{
    auto const target = http_target::parse("http://example.com/index.html?q=1#top").value();

    CHECK(target.host == "example.com");
    CHECK(target.port == 80);
    CHECK(!target.secure);

    // The fragment is parsed and never sent: it is resolved by the client, and a server has no business seeing it.
    CHECK(target.request_target() == "/index.html?q=1");
    CHECK(target.origin() == "http://example.com");
    CHECK(target.host_header() == "example.com");
}

TEST("cnet - https defaults to 443, and an explicit port is kept")
{
    auto const plain = http_target::parse("https://example.com/").value();
    CHECK(plain.port == 443);
    CHECK(plain.secure);
    CHECK(plain.origin() == "https://example.com");
    CHECK(plain.host_header() == "example.com");

    auto const with_port = http_target::parse("https://example.com:8443/x").value();
    CHECK(with_port.port == 8443);

    // A non-default port belongs in both the origin and the Host header; the default belongs in neither.
    CHECK(with_port.origin() == "https://example.com:8443");
    CHECK(with_port.host_header() == "example.com:8443");

    auto const redundant = http_target::parse("https://example.com:443/x").value();
    CHECK(redundant.origin() == "https://example.com");
}

TEST("cnet - an empty path becomes a slash")
{
    // `http://example.com` has no path at all in RFC 3986 terms, and the request line needs one.
    auto const target = http_target::parse("http://example.com").value();
    CHECK(target.request_target() == "/");

    auto const with_query = http_target::parse("http://example.com?q=1").value();
    CHECK(with_query.request_target() == "/?q=1");
}

TEST("cnet - the host is lower-cased and an IPv6 literal loses its brackets")
{
    auto const mixed = http_target::parse("http://ExAmPle.COM/Path").value();
    CHECK(mixed.host == "example.com");

    // The path keeps its case: only the scheme and host are case-insensitive.
    CHECK(mixed.request_target() == "/Path");

    auto const v6 = http_target::parse("http://[::1]:8080/").value();
    CHECK(v6.host == "::1");
    CHECK(v6.port == 8080);

    // The brackets come back where they are needed and stay off where they are not.
    CHECK(v6.host_header() == "[::1]:8080");
    CHECK(v6.origin() == "http://[::1]:8080");
}

TEST("cnet - credentials in a URL are refused rather than dropped")
{
    // `https://evil.com@good.com/` is a URL most readers get the host of wrong.
    // A client that silently ignores the first half is the reason that trick works, so this refuses instead.
    auto const with_credentials = http_target::parse("https://evil.com@good.com/");
    CHECK(with_credentials.has_error());
    CHECK(with_credentials.error().code == error_code::invalid_argument);

    auto const with_password = http_target::parse("https://user:secret@example.com/");
    CHECK(with_password.has_error());
}

TEST("cnet - what is not an http URL is refused")
{
    // A relative reference has nothing to connect to.
    CHECK(http_target::parse("/just/a/path").has_error());
    CHECK(http_target::parse("example.com/x").has_error());

    // A scheme this client cannot speak.
    CHECK(http_target::parse("ftp://example.com/x").has_error());
    CHECK(http_target::parse("file:///etc/passwd").has_error());

    // No host at all.
    CHECK(http_target::parse("http:///path").has_error());

    // A control character anywhere, which is how one URL becomes two.
    CHECK(http_target::parse("http://example.com/a\rb").has_error());
    CHECK(http_target::parse("http://example.com/a b").has_error());

    // A port that is not one.
    CHECK(http_target::parse("http://example.com:0/").has_error());
    CHECK(http_target::parse("http://example.com:99999/").has_error());
    CHECK(http_target::parse("http://example.com:http/").has_error());
}

TEST("cnet - a redirect is followed by resolving against the URL that was requested")
{
    auto const target = http_target::parse("https://example.com/a/b?q=1").value();

    // The whole point of keeping the cc::uri rather than only its pieces: RFC 3986 reference resolution is what a
    // Location header needs, and it is not something worth writing twice.
    auto const relative = target.url.resolve("../c").value();
    auto const followed = http_target::from_uri(relative).value();
    CHECK(followed.origin() == "https://example.com");
    CHECK(followed.request_target() == "/c");

    auto const absolute = target.url.resolve("https://other.example/x").value();
    auto const elsewhere = http_target::from_uri(absolute).value();
    CHECK(elsewhere.host == "other.example");

    // A redirect to a scheme this client does not speak is refused at the same gate as the original URL.
    auto const downgraded = target.url.resolve("ftp://example.com/x").value();
    CHECK(http_target::from_uri(downgraded).has_error());
}
