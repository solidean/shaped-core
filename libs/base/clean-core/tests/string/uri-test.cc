#include <clean-core/string/uri.hh>
#include <nexus/test.hh>

using namespace cc;

namespace
{
/// Resolve `reference` against RFC 3986 section 5.4's base and compare, which is what the section's tables are for.
cc::string resolved_against_rfc_base(cc::string_view reference)
{
    auto const base = uri::parse("http://a/b/c/d;p?q");
    CHECK(base.has_value());
    auto const r = base.value().resolve(reference);
    CHECK(r.has_value());
    return cc::string(r.value().text());
}
} // namespace

TEST("cc::uri - an absolute URI splits into its components")
{
    auto const u = uri_view::parse("https://user:pw@example.com:8443/a/b?x=1&y=2#frag");
    CHECK(u.has_value());

    auto const v = u.value();
    CHECK(v.is_absolute());
    CHECK(v.scheme() == "https");
    CHECK(v.has_authority());
    CHECK(v.authority() == "user:pw@example.com:8443");
    CHECK(v.has_userinfo());
    CHECK(v.userinfo() == "user:pw");
    CHECK(v.host() == "example.com");
    CHECK(v.has_port());
    CHECK(v.port_text() == "8443");
    CHECK(v.port().value() == 8443);
    CHECK(v.path() == "/a/b");
    CHECK(v.has_query());
    CHECK(v.query() == "x=1&y=2");
    CHECK(v.has_fragment());
    CHECK(v.fragment() == "frag");
}

TEST("cc::uri - absent and empty components are different")
{
    auto const none = uri_view::parse("http://h/p").value();
    CHECK(!none.has_query());
    CHECK(!none.has_fragment());
    CHECK(!none.has_userinfo());
    CHECK(!none.has_port());

    auto const empty = uri_view::parse("http://@h:/p?#").value();
    CHECK(empty.has_userinfo());
    CHECK(empty.userinfo() == "");
    CHECK(empty.has_port());
    CHECK(empty.port_text() == "");
    CHECK(!empty.port().has_value()); // present but empty, so there is no number to report
    CHECK(empty.has_query());
    CHECK(empty.query() == "");
    CHECK(empty.has_fragment());
    CHECK(empty.fragment() == "");
}

TEST("cc::uri - a relative reference has no scheme and no authority")
{
    auto const v = uri_view::parse("../images/tex.png").value();
    CHECK(!v.is_absolute());
    CHECK(v.scheme() == "");
    CHECK(!v.has_authority());
    CHECK(v.path() == "../images/tex.png");

    // A digit cannot start a scheme, so this is a path rather than a "1" scheme.
    auto const digit = uri_view::parse("1:2").value();
    CHECK(!digit.is_absolute());
    CHECK(digit.path() == "1:2");
}

TEST("cc::uri - a scheme with no authority keeps its whole remainder as the path")
{
    auto const v = uri_view::parse("mailto:someone@example.com").value();
    CHECK(v.is_absolute());
    CHECK(v.scheme() == "mailto");
    CHECK(!v.has_authority());
    CHECK(v.path() == "someone@example.com");

    auto const d = uri_view::parse("data:text/plain;base64,QUJD").value();
    CHECK(d.scheme() == "data");
    CHECK(d.path() == "text/plain;base64,QUJD");
}

TEST("cc::uri - an IPv6 literal is not read as a port list")
{
    auto const v = uri_view::parse("http://[2001:db8::1]:8080/p").value();
    CHECK(v.host() == "[2001:db8::1]");
    CHECK(v.port().value() == 8080);

    auto const no_port = uri_view::parse("http://[::1]/p").value();
    CHECK(no_port.host() == "[::1]");
    CHECK(!no_port.has_port());
    CHECK(no_port.path() == "/p");
}

TEST("cc::uri - the last @ ends the userinfo")
{
    auto const v = uri_view::parse("http://a%40b@host/p").value();
    CHECK(v.userinfo() == "a%40b");
    CHECK(v.host() == "host");
}

TEST("cc::uri - a ? inside a fragment is not a query")
{
    auto const v = uri_view::parse("http://h/p#frag?notquery").value();
    CHECK(!v.has_query());
    CHECK(v.path() == "/p");
    CHECK(v.fragment() == "frag?notquery");
}

TEST("cc::uri - malformed input is rejected rather than half-parsed")
{
    CHECK(!uri_view::parse("http://h/%zz").has_value());   // not hexadecimal
    CHECK(!uri_view::parse("http://h/%4").has_value());    // truncated
    CHECK(!uri_view::parse("http://h/a b").has_value());   // a raw space splits a URI in two
    CHECK(!uri_view::parse("http://h:80x/p").has_value()); // a port is digits or nothing
    CHECK(!uri_view::parse("http://[::1/p").has_value());  // unterminated IPv6 literal

    // An empty reference is legal, and so is an unknown scheme.
    CHECK(uri_view::parse("").has_value());
    CHECK(uri_view::parse("wibble://h/p").has_value());
}

TEST("cc::uri - percent encoding differs per component")
{
    CHECK(percent_encode("a/b c", uri_component::path) == "a/b%20c");
    CHECK(percent_encode("a/b c", uri_component::path_segment) == "a%2Fb%20c");
    CHECK(percent_encode("a?b", uri_component::query) == "a?b");
    CHECK(percent_encode("a?b", uri_component::path) == "a%3Fb");
    CHECK(percent_encode("a b+c", uri_component::form) == "a+b%2Bc");
    CHECK(percent_encode("a:b", uri_component::userinfo) == "a:b");
    CHECK(percent_encode("a:b", uri_component::host) == "a%3Ab");

    // The unreserved set survives every component, and only it.
    CHECK(percent_encode("-._~AZaz09", uri_component::form) == "-._~AZaz09");
}

TEST("cc::uri - decoding is strict about escapes and quiet about their bytes")
{
    CHECK(percent_decode("a%20b").value() == "a b");
    CHECK(percent_decode("%C3%A4").value() == "\xC3\xA4");
    CHECK(percent_decode("a+b").value() == "a+b"); // a plus is a plus outside form encoding
    CHECK(percent_decode_form("a+b").value() == "a b");
    CHECK(percent_decode_form("a%2Bb").value() == "a+b");

    CHECK(!percent_decode("%").has_value());
    CHECK(!percent_decode("%4").has_value());
    CHECK(!percent_decode("%zz").has_value());

    // Lower-case hex decodes, and round-trips back as upper case.
    CHECK(percent_decode("%c3").value() == "\xC3");
    CHECK(percent_encode("\xC3", uri_component::path) == "%C3");
}

TEST("cc::uri - query parameters split without decoding")
{
    auto const params = parse_query_parameters("a=1&b&c=&d=x%20y");
    CHECK(params.size() == 4);
    CHECK(params[0].name == "a");
    CHECK(params[0].value == "1");
    CHECK(params[0].has_value);
    CHECK(params[1].name == "b");
    CHECK(!params[1].has_value);
    CHECK(params[2].name == "c");
    CHECK(params[2].value == "");
    CHECK(params[2].has_value);
    CHECK(params[3].value == "x%20y");

    // Empty runs between separators are skipped rather than reported as nameless parameters.
    CHECK(parse_query_parameters("&&a=1&&").size() == 1);
    CHECK(parse_query_parameters("").size() == 0);

    // `;` is not a separator: one old HTML recommendation allowed it and no current one does.
    auto const semi = parse_query_parameters("a=1;b=2");
    CHECK(semi.size() == 1);
    CHECK(semi[0].value == "1;b=2");
}

TEST("cc::uri - finding one parameter allocates nothing")
{
    CHECK(find_query_parameter("a=1&b=2", "b").value() == "2");
    CHECK(find_query_parameter("a=1&b=2", "a").value() == "1");
    CHECK(find_query_parameter("a=1&b", "b").value() == "");
    CHECK(!find_query_parameter("a=1&b=2", "c").has_value());

    // The first wins, which is what a router should do with a duplicated parameter.
    CHECK(find_query_parameter("a=1&a=2", "a").value() == "1");
}

TEST("cc::uri - dot segments are removed, and .. cannot climb out")
{
    CHECK(remove_dot_segments("/a/b/c/./../../g") == "/a/g");
    CHECK(remove_dot_segments("mid/content=5/../6") == "mid/6");
    CHECK(remove_dot_segments("/a/b/../../../../c") == "/c");
    CHECK(remove_dot_segments("/./g") == "/g");
    CHECK(remove_dot_segments("/../g") == "/g");
    CHECK(remove_dot_segments("/a/b/") == "/a/b/");
    CHECK(remove_dot_segments("") == "");
    CHECK(remove_dot_segments("/") == "/");
}

TEST("cc::uri - RFC 3986 section 5.4.1 normal resolution examples")
{
    CHECK(resolved_against_rfc_base("g:h") == "g:h");
    CHECK(resolved_against_rfc_base("g") == "http://a/b/c/g");
    CHECK(resolved_against_rfc_base("./g") == "http://a/b/c/g");
    CHECK(resolved_against_rfc_base("g/") == "http://a/b/c/g/");
    CHECK(resolved_against_rfc_base("/g") == "http://a/g");
    CHECK(resolved_against_rfc_base("//g") == "http://g");
    CHECK(resolved_against_rfc_base("?y") == "http://a/b/c/d;p?y");
    CHECK(resolved_against_rfc_base("g?y") == "http://a/b/c/g?y");
    CHECK(resolved_against_rfc_base("#s") == "http://a/b/c/d;p?q#s");
    CHECK(resolved_against_rfc_base("g#s") == "http://a/b/c/g#s");
    CHECK(resolved_against_rfc_base("g?y#s") == "http://a/b/c/g?y#s");
    CHECK(resolved_against_rfc_base(";x") == "http://a/b/c/;x");
    CHECK(resolved_against_rfc_base("g;x") == "http://a/b/c/g;x");
    CHECK(resolved_against_rfc_base("g;x?y#s") == "http://a/b/c/g;x?y#s");
    CHECK(resolved_against_rfc_base("") == "http://a/b/c/d;p?q");
    CHECK(resolved_against_rfc_base(".") == "http://a/b/c/");
    CHECK(resolved_against_rfc_base("./") == "http://a/b/c/");
    CHECK(resolved_against_rfc_base("..") == "http://a/b/");
    CHECK(resolved_against_rfc_base("../") == "http://a/b/");
    CHECK(resolved_against_rfc_base("../g") == "http://a/b/g");
    CHECK(resolved_against_rfc_base("../..") == "http://a/");
    CHECK(resolved_against_rfc_base("../../") == "http://a/");
    CHECK(resolved_against_rfc_base("../../g") == "http://a/g");
}

TEST("cc::uri - RFC 3986 section 5.4.2 abnormal resolution examples")
{
    CHECK(resolved_against_rfc_base("../../../g") == "http://a/g");
    CHECK(resolved_against_rfc_base("../../../../g") == "http://a/g");
    CHECK(resolved_against_rfc_base("/./g") == "http://a/g");
    CHECK(resolved_against_rfc_base("/../g") == "http://a/g");
    CHECK(resolved_against_rfc_base("g.") == "http://a/b/c/g.");
    CHECK(resolved_against_rfc_base(".g") == "http://a/b/c/.g");
    CHECK(resolved_against_rfc_base("g..") == "http://a/b/c/g..");
    CHECK(resolved_against_rfc_base("..g") == "http://a/b/c/..g");
    CHECK(resolved_against_rfc_base("./../g") == "http://a/b/g");
    CHECK(resolved_against_rfc_base("./g/.") == "http://a/b/c/g/");
    CHECK(resolved_against_rfc_base("g/./h") == "http://a/b/c/g/h");
    CHECK(resolved_against_rfc_base("g/../h") == "http://a/b/c/h");
    CHECK(resolved_against_rfc_base("g;x=1/./y") == "http://a/b/c/g;x=1/y");
    CHECK(resolved_against_rfc_base("g;x=1/../y") == "http://a/b/c/y");
    CHECK(resolved_against_rfc_base("g?y/./x") == "http://a/b/c/g?y/./x");
    CHECK(resolved_against_rfc_base("g?y/../x") == "http://a/b/c/g?y/../x");
    CHECK(resolved_against_rfc_base("g#s/./x") == "http://a/b/c/g#s/./x");
    CHECK(resolved_against_rfc_base("g#s/../x") == "http://a/b/c/g#s/../x");

    // Strict parsing: a reference carrying the base's own scheme is still an absolute reference.
    CHECK(resolved_against_rfc_base("http:g") == "http:g");
}

TEST("cc::uri - resolution needs an absolute base")
{
    auto const relative = uri::parse("b/c").value();
    CHECK(!relative.resolve("d").has_value());
}

TEST("cc::uri - normalization touches case, escapes and dot segments only")
{
    auto const u = uri::parse("HTTP://User@ExAmPle.COM:80/a/./b/../c/%7Eu%2Fv?Q=%2a#F%7e").value();
    auto const n = u.normalized();
    CHECK(n.text() == "http://User@example.com:80/a/c/~u%2Fv?Q=%2A#F~");

    // Deliberately NOT done, because both are http rules rather than URI rules:
    CHECK(n.port().value() == 80);    // the default port is kept
    CHECK(n.path() == "/a/c/~u%2Fv"); // and an escaped '/' stays escaped, since decoding it would invent a segment

    // Normalization is idempotent.
    CHECK(n.normalized().text() == n.text());
}

TEST("cc::uri - the owning form survives what the view was parsed from")
{
    auto owned = uri();
    {
        auto const source = cc::string("https://example.com/a?b#c");
        owned = uri::parse(source).value();
    }
    CHECK(owned.scheme() == "https");
    CHECK(owned.host() == "example.com");
    CHECK(owned.path() == "/a");
    CHECK(owned.query() == "b");
    CHECK(owned.fragment() == "c");

    // A move rebases the view rather than dangling it, which is why cc::uri stores offsets and not a view.
    auto const moved = cc::move(owned);
    CHECK(moved.host() == "example.com");
    CHECK(moved.view().host() == "example.com");
}
