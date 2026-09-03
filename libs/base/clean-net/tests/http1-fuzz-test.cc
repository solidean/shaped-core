#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-net/http/impl/http1.hh>
#include <nexus/fuzz/fuzz.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// The parser is the one thing in this library that reads bytes from outside the process, so it is the one that earns a
// fuzzer.
//
// WHAT IS BEING CHECKED IS NOT "DOES IT CRASH".
// A crash would be found by running it, and a parser that merely does not crash can still be a request smuggling
// primitive: the failure that matters is two readers disagreeing about where a message ends.
// The invariant here is the one that rules that out -- **the same bytes must parse the same way however they are
// split** -- because a chunk boundary is exactly what a parser sees differently from anybody reasoning about the
// message as a whole.

namespace
{
/// Everything about a parse that a caller can observe.
struct parse_outcome
{
    bool failed = false;
    bool head_complete = false;
    bool message_complete = false;
    bool reusable = false;
    i32 status = 0;
    isize header_count = 0;
    cc::string body;

    [[nodiscard]] bool operator==(parse_outcome const& o) const
    {
        return failed == o.failed && head_complete == o.head_complete && message_complete == o.message_complete
            && reusable == o.reusable && status == o.status && header_count == o.header_count && body == o.body;
    }
};

/// Feed `text` to a fresh parser `chunk_size` bytes at a time, as a socket would.
///
/// `overran` is set if the parser ever claims to have consumed more than it was offered, which would corrupt the
/// caller's buffer rather than merely misread the message.
[[nodiscard]] parse_outcome parse_in_chunks(cc::string_view text, isize chunk_size, bool& overran)
{
    auto out = parse_outcome();

    auto parser = impl::http1_parser();
    parser.start_response(http_method::get);

    auto buffer = cc::vector<byte>();
    auto offset = isize(0);

    auto sink = [&out](cc::span<byte const> chunk) -> isize
    {
        for (auto const b : chunk)
            out.body += char(b);
        return chunk.size();
    };

    while (!parser.message_complete() && !out.failed)
    {
        if (offset < text.size())
        {
            auto const take = text.size() - offset < chunk_size ? text.size() - offset : chunk_size;
            for (isize i = 0; i < take; ++i)
                buffer.push_back(byte(text[offset + i]));
            offset += take;
        }

        // Whatever is here is offered until the parser stops taking it, which is either "the head is ready" or "that
        // is all this holds".
        while (!buffer.empty() && !parser.message_complete())
        {
            auto const fed = parser.feed(cc::span<byte const>(buffer.data(), buffer.size()), sink);
            if (fed.has_error())
            {
                out.failed = true;
                break;
            }

            if (fed.value() > buffer.size())
            {
                overran = true;
                out.failed = true;
                break;
            }

            if (fed.value() == 0)
                break;

            auto rest = cc::vector<byte>();
            for (auto i = fed.value(); i < buffer.size(); ++i)
                rest.push_back(buffer[i]);
            buffer = cc::move(rest);
        }

        if (offset >= text.size())
            break;
    }

    // A body delimited by the close is finished by the close, and a body cut short by one is truncated -- and which of
    // the two it was must not depend on the chunking either.
    if (!out.failed && !parser.message_complete())
        out.failed = parser.notify_end_of_stream().has_error();

    out.head_complete = parser.head_complete();
    out.message_complete = parser.message_complete();
    if (out.head_complete)
    {
        out.reusable = parser.can_reuse_connection();
        out.status = parser.response().status;
        out.header_count = parser.response().headers.size();
    }
    return out;
}

/// The invariant: every chunking of the same bytes reaches the same conclusion.
[[nodiscard]] bool parses_the_same_however_split(cc::string const& text)
{
    if (text.empty())
        return true;

    auto overran = false;
    auto const whole = parse_in_chunks(text, text.size(), overran);

    for (auto const chunk_size : {isize(1), isize(2), isize(3), isize(7), isize(64)})
    {
        auto const split = parse_in_chunks(text, chunk_size, overran);
        if (overran || !(split == whole))
            return false;
    }
    return true;
}

/// Keep a generated message small enough that the byte-at-a-time pass stays cheap.
constexpr isize k_max_message = 256;
} // namespace

TEST("cnet - the http/1.1 parser reads the same message however it is split")
{
    auto test = nx::fuzz::test::create();

    // The pieces a response is made of, plus the ones that are only interesting because they are wrong.
    test->add_value("status-200", cc::string("HTTP/1.1 200 OK\r\n"));
    test->add_value("status-204", cc::string("HTTP/1.1 204 No Content\r\n"));
    test->add_value("status-http10", cc::string("HTTP/1.0 200 OK\r\n"));
    test->add_value("length-5", cc::string("Content-Length: 5\r\n"));
    test->add_value("length-0", cc::string("Content-Length: 0\r\n"));
    test->add_value("chunked", cc::string("Transfer-Encoding: chunked\r\n"));
    test->add_value("close", cc::string("Connection: close\r\n"));
    test->add_value("header", cc::string("X-Thing: value\r\n"));
    test->add_value("blank", cc::string("\r\n"));
    test->add_value("body", cc::string("hello"));
    test->add_value("chunk-body", cc::string("5\r\nhello\r\n0\r\n\r\n"));
    test->add_value("bare-lf", cc::string("X-Thing: value\n"));
    test->add_value("obs-fold", cc::string("X-Thing: value\r\n \tmore\r\n"));
    test->add_value("junk", cc::string("\r\n\r\n\x7f\x01 : \r"));

    test->add_op("concat",
                 [](cc::string a, cc::string b)
                 {
                     if (a.size() + b.size() > k_max_message)
                         return a;
                     return a + b;
                 });

    test->add_op("flip-a-byte",
                 [](cc::string a)
                 {
                     if (a.empty())
                         return a;

                     // A deterministic position, since the engine's own randomness is what varies the input.
                     auto const at = a.size() / 2;
                     a[at] = char(u8(a[at]) ^ 0x20);
                     return a;
                 });

    test->add_op("truncate",
                 [](cc::string a)
                 {
                     if (a.size() < 2)
                         return a;
                     return cc::string(cc::string_view(a).subview({.offset = 0, .size = a.size() - 1}));
                 });

    test->add_invariant("chunk-independent", [](cc::string const& s) { return parses_the_same_however_split(s); });

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }
}

TEST("cnet - the chunking invariant holds for the messages we already know are interesting")
{
    // The fuzzer explores; these are the shapes that are worth pinning whatever it happens to find, and they run
    // whether or not the fuzz section does.
    auto const messages = cc::vector<cc::string>{
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 204 No Content\r\n\r\n",
        "HTTP/1.1 200 OK\r\n\r\nuntil the close",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel",                                 // truncated
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello", // both, which is smuggling
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello",
        "HTTP/1.0 200 OK\r\n\r\nno framing at all",
        "HTTP/1.1 200 OK\r\nX-A: 1\r\nX-A: 2\r\n\r\n",
        "garbage\r\n\r\n",
        "\r\n",
    };

    for (auto const& message : messages)
        CHECK(parses_the_same_however_split(message));
}
