#include "stream-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/streams/read_line.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc_stream_test;

namespace
{
cc::span<cc::byte const> as_bytes(cc::string_view s)
{
    return cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(s.data()), s.size());
}

/// Drain a stream into one line per element until end of data.
template <class Stream>
cc::vector<cc::string> collect_lines(Stream& s)
{
    cc::vector<cc::string> lines;
    auto line = cc::string();
    while (true)
    {
        auto r = cc::read_line(s, line);
        REQUIRE(r.has_value());
        if (!r.value())
            break;
        lines.push_back(line);
    }
    return lines;
}
} // namespace

TEST("read_line - basic lines, final line without newline")
{
    auto const text = cc::string_view("alpha\nbeta\ngamma");
    cc::span_read_stream_adapter adapter{as_bytes(text)};
    cc::read_stream s = adapter;

    auto const lines = collect_lines(s);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "alpha");
    CHECK(lines[1] == "beta");
    CHECK(lines[2] == "gamma"); // no trailing newline: still returned once
}

TEST("read_line - trailing newline adds no phantom line")
{
    auto const text = cc::string_view("a\nb\n");
    cc::span_read_stream_adapter adapter{as_bytes(text)};
    cc::read_stream s = adapter;

    auto const lines = collect_lines(s);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "a");
    CHECK(lines[1] == "b");
}

TEST("read_line - empty lines are preserved")
{
    auto const text = cc::string_view("\n\nx\n");
    cc::span_read_stream_adapter adapter{as_bytes(text)};
    cc::read_stream s = adapter;

    auto const lines = collect_lines(s);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "");
    CHECK(lines[1] == "");
    CHECK(lines[2] == "x");
}

TEST("read_line - CRLF endings are stripped")
{
    auto const text = cc::string_view("one\r\ntwo\r\nthree\r\n");
    cc::span_read_stream_adapter adapter{as_bytes(text)};
    cc::read_stream s = adapter;

    auto const lines = collect_lines(s);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "one");
    CHECK(lines[1] == "two");
    CHECK(lines[2] == "three");
}

TEST("read_line - empty input yields no line and clears out")
{
    cc::span_read_stream_adapter adapter{cc::span<cc::byte const>()};
    cc::read_stream s = adapter;

    auto line = cc::string("prefilled");
    auto r = cc::read_line(s, line);
    REQUIRE(r.has_value());
    CHECK(!r.value());
    CHECK(line == ""); // cleared even when nothing is read
}

TEST("read_line - lines and CRLF split across buffer refills")
{
    // A chunked pipe forces multi-window reads; small chunks split "\r\n" across a window boundary, so the
    // trailing-'\r' strip must act on bytes already copied into out from an earlier window.
    auto const text = cc::string_view("hello\r\nfrom\na chunked\r\npipe");
    for (auto const chunk : {cc::isize(1), cc::isize(2), cc::isize(3), cc::isize(7)})
    {
        mock_pipe_read_stream_adapter adapter{as_bytes(text), chunk};
        cc::read_stream s = adapter.stream();

        auto const lines = collect_lines(s);
        REQUIRE(lines.size() == 4);
        CHECK(lines[0] == "hello");
        CHECK(lines[1] == "from");
        CHECK(lines[2] == "a chunked");
        CHECK(lines[3] == "pipe");
    }
}
