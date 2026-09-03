#include <clean-core/container/vector.hh>
#include <clean-net/http/impl/http1.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// The wire format, fed by hand.
//
// No connection, no reactor: the parser takes bytes and the serializer produces them, so every case here is exact.
// The refusals matter more than the successes -- each one is a way for this client and a server to disagree about
// where a message ends, which is what request smuggling is made of.

namespace
{
[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view text)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size());
}

/// A sink that takes everything, and remembers it.
struct collecting_sink
{
    cc::vector<byte> collected;

    [[nodiscard]] isize operator()(cc::span<byte const> chunk)
    {
        for (auto const b : chunk)
            collected.push_back(b);
        return chunk.size();
    }

    [[nodiscard]] cc::string_view text() const
    {
        return cc::string_view(reinterpret_cast<char const*>(collected.data()), collected.size());
    }
};

/// Feed a whole response in one go, and report what happened.
struct parse_outcome
{
    bool ok = false;
    isize consumed = 0;
    cc::string body;
};

[[nodiscard]] parse_outcome parse_all(impl::http1_response_parser& parser, cc::string_view wire)
{
    auto sink = collecting_sink();
    auto outcome = parse_outcome();

    auto const input = bytes_of(wire);
    auto cursor = isize(0);

    // The parser stops at the end of the head so a caller can look at it, so feeding is a loop rather than one call.
    while (cursor < input.size() && !parser.message_complete())
    {
        auto const fed = parser.feed(input.subspan(cursor), [&sink](cc::span<byte const> c) { return sink(c); });
        if (fed.has_error())
            return outcome;

        if (fed.value() == 0)
            break;
        cursor += fed.value();
    }

    outcome.ok = true;
    outcome.consumed = cursor;
    outcome.body = cc::string(sink.text());
    return outcome;
}
} // namespace

TEST("cnet - a response with a content length parses")
{
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto const outcome = parse_all(parser, "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/plain\r\n"
                                           "Content-Length: 5\r\n"
                                           "\r\n"
                                           "hello");
    CHECK(outcome.ok);
    CHECK(parser.message_complete());
    CHECK(parser.head().status == 200);
    CHECK(parser.head().reason == "OK");
    CHECK(parser.head().headers.get("content-type").value() == "text/plain");
    CHECK(outcome.body == "hello");
    CHECK(parser.can_reuse_connection());
}

TEST("cnet - a chunked response is reassembled")
{
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto const outcome = parse_all(parser, "HTTP/1.1 200 OK\r\n"
                                           "Transfer-Encoding: chunked\r\n"
                                           "\r\n"
                                           "5\r\nhello\r\n"
                                           "7;ext=1\r\n, world\r\n"
                                           "0\r\n"
                                           "\r\n");
    CHECK(outcome.ok);
    CHECK(parser.message_complete());
    CHECK(outcome.body == "hello, world");
    CHECK(parser.body_bytes() == 12);

    // A chunk extension is legal and carries nothing anybody uses, so it parses and is ignored.
    CHECK(parser.can_reuse_connection());
}

TEST("cnet - trailers are read and the message ends after them")
{
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto const outcome = parse_all(parser, "HTTP/1.1 200 OK\r\n"
                                           "Transfer-Encoding: chunked\r\n"
                                           "\r\n"
                                           "4\r\nbody\r\n"
                                           "0\r\n"
                                           "X-Checksum: 1234\r\n"
                                           "\r\n");
    CHECK(outcome.ok);
    CHECK(parser.message_complete());
    CHECK(outcome.body == "body");
}

TEST("cnet - a response arriving one byte at a time parses the same")
{
    auto const wire = cc::string_view("HTTP/1.1 404 Not Found\r\n"
                                      "Content-Length: 3\r\n"
                                      "\r\n"
                                      "abc");

    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto sink = collecting_sink();
    auto const input = bytes_of(wire);

    // The shape a real network produces and loopback never does: one byte per read.
    for (isize i = 0; i < input.size(); ++i)
    {
        auto const fed
            = parser.feed(input.subspan({.offset = i, .size = 1}), [&sink](cc::span<byte const> c) { return sink(c); });
        CHECK(fed.has_value());
    }

    CHECK(parser.message_complete());
    CHECK(parser.head().status == 404);
    CHECK(parser.head().reason == "Not Found");
    CHECK(sink.text() == "abc");
}

TEST("cnet - a sink that takes less stops the parser there")
{
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto const wire = cc::string_view("HTTP/1.1 200 OK\r\n"
                                      "Content-Length: 10\r\n"
                                      "\r\n"
                                      "0123456789");
    auto const input = bytes_of(wire);

    auto cursor = isize(0);
    cursor += parser.feed(input, [](cc::span<byte const>) { return isize(0); }).value();

    // The head is in and the body has not started, because the sink refused it.
    CHECK(parser.head_complete());
    CHECK(!parser.message_complete());

    auto const before_body = cursor;
    cursor += parser.feed(input.subspan(cursor), [](cc::span<byte const>) { return isize(0); }).value();
    CHECK(cursor == before_body);

    // Backpressure is per call: taking four bytes at a time gets the body across in pieces.
    auto taken = cc::string();
    while (!parser.message_complete())
    {
        auto const fed = parser.feed(input.subspan(cursor),
                                     [&taken](cc::span<byte const> chunk)
                                     {
                                         auto const n = chunk.size() < 4 ? chunk.size() : isize(4);
                                         taken += cc::string_view(reinterpret_cast<char const*>(chunk.data()), n);
                                         return n;
                                     });
        CHECK(fed.has_value());
        cursor += fed.value();
    }

    CHECK(taken == "0123456789");
}

TEST("cnet - the responses that have no body are known without one")
{
    // A HEAD response carries a Content-Length describing the body it is NOT sending.
    auto head_parser = impl::http1_response_parser();
    head_parser.start(http_method::head);
    auto const head_outcome = parse_all(head_parser, "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n");
    CHECK(head_outcome.ok);
    CHECK(head_parser.message_complete());
    CHECK(head_outcome.body.empty());

    for (auto const status : {"204 No Content", "304 Not Modified"})
    {
        auto parser = impl::http1_response_parser();
        parser.start(http_method::get);
        auto const outcome = parse_all(parser, cc::format("HTTP/1.1 {}\r\nContent-Length: 5\r\n\r\n", status));
        CHECK(outcome.ok);
        CHECK(parser.message_complete());
        CHECK(outcome.body.empty());
    }
}

TEST("cnet - a body delimited by the close ends there, and the connection cannot be reused")
{
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto const outcome = parse_all(parser, "HTTP/1.0 200 OK\r\n"
                                           "\r\n"
                                           "everything until the end");
    CHECK(outcome.ok);
    CHECK(!parser.message_complete());
    CHECK(outcome.body == "everything until the end");

    // The close is the framing, so it completes the message -- and there is nothing left to reuse.
    CHECK(parser.notify_end_of_stream().has_value());
    CHECK(parser.message_complete());
    CHECK(!parser.can_reuse_connection());
}

TEST("cnet - a close in the middle of a counted body is a truncation")
{
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto const outcome = parse_all(parser, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly this much");
    CHECK(outcome.ok);
    CHECK(!parser.message_complete());

    // The difference that matters: this one is an error, and the close-delimited one above is not.
    auto const ended = parser.notify_end_of_stream();
    CHECK(ended.has_error());
    CHECK(ended.error().code == error_code::protocol_error);
}

TEST("cnet - a message framed two ways at once is refused")
{
    // The classic request smuggling primitive: one party reads the length and the other reads the chunks.
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto const wire = cc::string_view("HTTP/1.1 200 OK\r\n"
                                      "Content-Length: 5\r\n"
                                      "Transfer-Encoding: chunked\r\n"
                                      "\r\n");
    auto const fed = parser.feed(bytes_of(wire), [](cc::span<byte const> c) { return c.size(); });
    CHECK(fed.has_error());
    CHECK(fed.error().code == error_code::protocol_error);

    auto twice = impl::http1_response_parser();
    twice.start(http_method::get);
    auto const two_lengths = twice.feed(bytes_of("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"),
                                        [](cc::span<byte const> c) { return c.size(); });
    CHECK(two_lengths.has_error());
}

TEST("cnet - malformed heads are refused rather than repaired")
{
    auto const bad = {
        cc::string_view("HTTP/1.1 200 OK\nContent-Length: 0\r\n\r\n"),       // a bare LF line ending
        cc::string_view("HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n"),    // a space before the colon
        cc::string_view("HTTP/1.1 200 OK\r\nX-A: 1\r\n  continued\r\n\r\n"), // an obs-fold continuation
        cc::string_view("HTTP/1.1 20 OK\r\n\r\n"),                           // a status code that is not three digits
        cc::string_view("HTTP/1.1 999 Nope\r\n\r\n"),                        // outside 100-599
        cc::string_view("HTTP/2.0 200 OK\r\n\r\n"),                          // a version this parser does not speak
        cc::string_view("ICY 200 OK\r\n\r\n"),                               // not an HTTP status line at all
        cc::string_view("HTTP/1.1 200 OK\r\n: novalue\r\n\r\n"),             // a header with no name
    };

    for (auto const wire : bad)
    {
        auto parser = impl::http1_response_parser();
        parser.start(http_method::get);

        auto const fed = parser.feed(bytes_of(wire), [](cc::span<byte const> c) { return c.size(); });
        CHECK(fed.has_error());
    }
}

TEST("cnet - a chunk size that is not a number is refused")
{
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get);

    auto const wire = cc::string_view("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
    auto const input = bytes_of(wire);

    auto cursor = parser.feed(input, [](cc::span<byte const> c) { return c.size(); }).value();
    auto const fed = parser.feed(input.subspan(cursor), [](cc::span<byte const> c) { return c.size(); });
    CHECK(fed.has_error());
}

TEST("cnet - Connection close is honoured, and keep-alive revives an HTTP/1.0 connection")
{
    auto closing = impl::http1_response_parser();
    closing.start(http_method::get);
    CHECK(parse_all(closing, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").ok);
    CHECK(closing.message_complete());
    CHECK(!closing.can_reuse_connection());

    // A list rather than a single token, which is how Connection is actually written.
    auto listed = impl::http1_response_parser();
    listed.start(http_method::get);
    CHECK(parse_all(listed, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive, close\r\n\r\n").ok);
    CHECK(!listed.can_reuse_connection());

    // HTTP/1.0 closes by default, and says so when it does not.
    auto old_default = impl::http1_response_parser();
    old_default.start(http_method::get);
    CHECK(parse_all(old_default, "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n").ok);
    CHECK(!old_default.can_reuse_connection());

    auto old_keep_alive = impl::http1_response_parser();
    old_keep_alive.start(http_method::get);
    CHECK(parse_all(old_keep_alive, "HTTP/1.0 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n").ok);
    CHECK(old_keep_alive.can_reuse_connection());
}

TEST("cnet - a head bigger than the limit is refused rather than buffered")
{
    auto parser = impl::http1_response_parser();
    parser.start(http_method::get, {.max_status_line_bytes = 64, .max_header_bytes = 128, .max_header_count = 4});

    auto wire = cc::string("HTTP/1.1 200 OK\r\n");
    for (auto i = 0; i < 10; ++i)
        wire += cc::format("X-Padding-{}: {}\r\n", i, cc::string_view("0123456789"));
    wire += "\r\n";

    // A server that sends headers forever is indistinguishable from a slow one, until something says stop.
    auto const fed = parser.feed(bytes_of(wire), [](cc::span<byte const> c) { return c.size(); });
    CHECK(fed.has_error());
    CHECK(fed.error().code == error_code::protocol_error);
}

TEST("cnet - a request head is written the way a server expects to read it")
{
    auto request = http_request();
    request.method = http_method::get;
    request.target = http_target::parse("http://example.com/index.html?q=1").value();
    request.headers.add("Accept", "text/plain");

    auto const head = impl::write_request_head(request, true).value();
    CHECK(head
          == "GET /index.html?q=1 HTTP/1.1\r\n"
             "Accept: text/plain\r\n"
             "Host: example.com\r\n"
             "\r\n");

    // The Host comes from the target when the caller did not say, and stays the caller's when they did.
    request.headers.set("Host", "override.example");
    auto const overridden = impl::write_request_head(request, false).value();
    CHECK(overridden.contains("Host: override.example\r\n"));
    CHECK(!overridden.contains("Host: example.com"));
    CHECK(overridden.contains("Connection: close\r\n"));
}

TEST("cnet - a header that would end the head early cannot be sent")
{
    auto request = http_request();
    request.target = http_target::parse("http://example.com/").value();

    // The whole reason the check lives in the serializer: this is the last point before the bytes leave, and one
    // check there cannot be bypassed by a caller who built the header some other way.
    request.headers.add("X-Evil", "value\r\nInjected: yes");
    CHECK(impl::write_request_head(request, true).has_error());

    request.headers.clear();
    request.headers.add("X-Evil\r\nInjected", "value");
    CHECK(impl::write_request_head(request, true).has_error());

    request.headers.clear();
    request.headers.add("Bad Name", "value");
    CHECK(impl::write_request_head(request, true).has_error());

    request.headers.clear();
    request.headers.add("X-Fine", "value");
    CHECK(impl::write_request_head(request, true).has_value());
}
