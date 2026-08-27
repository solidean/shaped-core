#include <clean-core/bytes/compression.hh>
#include <clean-core/bytes/compression_stream.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
using algo = cc::compression_algorithm;
using framing = cc::compression_framing;

constexpr algo all_algorithms[] = {algo::zstd, algo::lz4, algo::deflate};

[[nodiscard]] cc::vector<byte> line_payload(isize lines)
{
    auto const text = cc::string_view("the quick brown fox jumps over the lazy dog\n");

    auto out = cc::vector<byte>();
    for (isize i = 0; i < lines; ++i)
        for (isize c = 0; c < text.size(); ++c)
            out.push_back(byte(u8(text[c])));
    return out;
}

[[nodiscard]] bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

/// Compress `payload` through the streaming writer, in chunks, and return the frame it produced.
[[nodiscard]] cc::vector<byte> stream_compress(cc::span<byte const> payload, algo a, isize chunk, framing f = framing::frame)
{
    auto sink = cc::vector<byte>::create_uninitialized(payload.size() * 2 + 4096);
    auto sink_adapter = cc::span_write_stream_adapter(sink);

    auto writer = cc::compressing_write_stream_adapter::create(sink_adapter, {.algorithm = a, .framing = f});
    REQUIRE(writer.has_value());

    {
        cc::write_stream out = writer.value().stream();
        for (isize at = 0; at < payload.size(); at += chunk)
        {
            auto const n = cc::min(chunk, payload.size() - at);
            REQUIRE(out.write(payload.subspan({.offset = at, .size = n})).has_value());
        }
        REQUIRE(out.flush().has_value());
    }

    auto const total = writer.value().finish();
    REQUIRE(total.has_value());
    REQUIRE(sink_adapter.stream().flush().has_value());

    sink.resize_down_to(total.value());
    return sink;
}
} // namespace

TEST("compression stream - round trip through both adapters")
{
    auto const payload = line_payload(400);

    for (auto const a : all_algorithms)
    {
        auto const blob = stream_compress(payload, a, 777); // a chunk that straddles the 16 kB buffer

        auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
        auto reader = cc::decompressing_read_stream_adapter::create(source, {.algorithm = a});
        REQUIRE(reader.has_value());

        cc::read_stream in = reader.value().stream();
        auto const back = in.read_all();
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression stream - a streamed frame is readable by the one-shot API")
{
    // The adapters are not a private encoding: what they write is an ordinary frame, which is what makes them
    // interchangeable with cc::compress at a format boundary.
    auto const payload = line_payload(50);

    for (auto const a : all_algorithms)
    {
        auto const blob = stream_compress(payload, a, 4096);
        auto const back = cc::decompress(blob);
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression stream - a one-shot frame is readable by the adapter")
{
    auto const payload = line_payload(50);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});

        auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
        auto reader = cc::decompressing_read_stream_adapter::create(source, {.algorithm = a});
        REQUIRE(reader.has_value());

        cc::read_stream in = reader.value().stream();
        auto const back = in.read_all();
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression stream - driven by read_line")
{
    // The first of the three ways babel drives a read_stream, and the one that leans entirely on the engine.
    auto const payload = line_payload(200);
    auto const blob = stream_compress(payload, algo::zstd, 1024);

    auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
    auto reader = cc::decompressing_read_stream_adapter::create(source, {.algorithm = algo::zstd});
    REQUIRE(reader.has_value());

    cc::read_stream in = reader.value().stream();
    auto line = cc::string();
    auto lines = isize(0);

    while (true)
    {
        auto const more = in.read_line(line);
        REQUIRE(more.has_value());
        if (!more.value())
            break;

        CHECK(line == "the quick brown fox jumps over the lazy dog");
        ++lines;
    }

    CHECK(lines == 200);
}

TEST("compression stream - driven by a manual ready_bytes / consume / flush loop")
{
    // The second way, and the one that stresses the window contract hardest: it consumes a window fully, flushes,
    // and re-reads ready_bytes() afterwards, so a refill that relocates the data has to be handled.
    auto const payload = line_payload(300);
    auto const blob = stream_compress(payload, algo::lz4, 512);

    auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
    auto reader = cc::decompressing_read_stream_adapter::create(source, {.algorithm = algo::lz4});
    REQUIRE(reader.has_value());

    cc::read_stream in = reader.value().stream();
    auto seen = cc::vector<byte>();

    while (true)
    {
        auto window = in.ready_bytes();
        if (window.empty())
        {
            REQUIRE(in.flush().has_value());
            window = in.ready_bytes();
            if (window.empty())
                break; // genuine end of data, and the only way this loop terminates
        }

        seen.push_back_range(window);
        in.consume(window.size());
    }

    CHECK(same_bytes(seen, payload));
}

TEST("compression stream - a decompressing stream is not seekable")
{
    auto const payload = line_payload(20);
    auto const blob = stream_compress(payload, algo::zstd, 4096);

    auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
    auto reader = cc::decompressing_read_stream_adapter::create(source, {.algorithm = algo::zstd});
    REQUIRE(reader.has_value());

    cc::read_stream in = reader.value().stream();
    auto upgraded = cc::move(in).try_as_seekable();
    CHECK(!upgraded.has_value());
    CHECK(in.is_valid());
}

TEST("compression stream - a zstd frame's declared size reaches read_all")
{
    // A one-shot zstd frame declares its content size, and the adapter reports it through remaining_size_hint —
    // which is the whole reason seek_dir carries that entry.
    auto const payload = line_payload(120);
    auto const blob = cc::compress(payload, {.algorithm = algo::zstd});

    auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
    auto reader = cc::decompressing_read_stream_adapter::create(source, {.algorithm = algo::zstd});
    REQUIRE(reader.has_value());

    cc::read_stream in = reader.value().stream();
    auto const back = in.read_all();
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), payload));
}

TEST("compression stream - truncated input is an error, not a hang")
{
    auto const payload = line_payload(400);

    for (auto const a : all_algorithms)
    {
        auto blob = stream_compress(payload, a, 4096);
        blob.resize_down_to(blob.size() / 2);

        auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
        auto reader = cc::decompressing_read_stream_adapter::create(source, {.algorithm = a});
        REQUIRE(reader.has_value());

        // The frame ends mid-stream.
        // What matters is that the refill reports end-of-data rather than spinning forever on a flush that produces nothing.
        cc::read_stream in = reader.value().stream();
        auto const back = in.read_all();
        auto const short_or_failed = back.has_error() || back.value().size() < payload.size();
        CHECK(short_or_failed);
    }
}

TEST("compression stream - raw framing has nothing to stream")
{
    auto sink = cc::vector<byte>::create_uninitialized(4096);
    auto sink_adapter = cc::span_write_stream_adapter(sink);
    CHECK(cc::compressing_write_stream_adapter::create(sink_adapter, {.framing = framing::raw}).has_error());

    auto source = cc::span_read_stream_adapter(cc::span<byte const>(sink));
    CHECK(cc::decompressing_read_stream_adapter::create(source, {.algorithm = algo::zstd, .framing = framing::raw})
              .has_error());
}

TEST("compression stream - deflate's zlib framing streams like gzip does")
{
    // `raw` is the only framing without a wrapper to resume into; the zlib wrapper is a header and a trailing
    // Adler-32, so it streams exactly as a gzip frame does.
    auto const payload = line_payload(400);
    auto const blob = stream_compress(payload, algo::deflate, 777, framing::zlib);

    auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
    auto reader
        = cc::decompressing_read_stream_adapter::create(source, {.algorithm = algo::deflate, .framing = framing::zlib});
    REQUIRE(reader.has_value());

    cc::read_stream in = reader.value().stream();
    auto const back = in.read_all();
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), payload));
}

TEST("compression stream - a streamed gzip frame is a gzip file")
{
    // The streaming writer emits the same container the one-shot path does, which is what lets it hand bytes to
    // something that only speaks gzip.
    auto const blob = stream_compress(line_payload(50), algo::deflate, 512);

    REQUIRE(blob.size() > 18);
    CHECK(u8(blob[0]) == 0x1f);
    CHECK(u8(blob[1]) == 0x8b);
    CHECK(u8(blob[2]) == 0x08);
}

TEST("compression stream - a decompressing stream must be told its algorithm")
{
    // Nothing has been read yet, so there is nothing to sniff — the context has to exist before the first byte moves.
    auto const blob = cc::compress(line_payload(10), {.algorithm = algo::zstd});
    auto source = cc::span_read_stream_adapter(cc::span<byte const>(blob));
    CHECK(cc::decompressing_read_stream_adapter::create(source, {}).has_error());
}

TEST("compression stream - finish is idempotent and reports the frame size")
{
    auto const payload = line_payload(30);
    auto sink = cc::vector<byte>::create_uninitialized(65536);
    auto sink_adapter = cc::span_write_stream_adapter(sink);

    auto writer = cc::compressing_write_stream_adapter::create(sink_adapter, {.algorithm = algo::zstd});
    REQUIRE(writer.has_value());

    {
        cc::write_stream out = writer.value().stream();
        REQUIRE(out.write(payload).has_value());
        REQUIRE(out.flush().has_value());
    }

    auto const first = writer.value().finish();
    REQUIRE(first.has_value());
    CHECK(first.value() > 0);

    auto const second = writer.value().finish();
    REQUIRE(second.has_value());
    CHECK(second.value() == first.value());
}

TEST("compression stream - an empty payload still produces a readable frame")
{
    for (auto const a : all_algorithms)
    {
        auto const blob = stream_compress({}, a, 1024);
        CHECK(blob.size() > 0); // a header and a terminator, even with nothing between them

        auto const back = cc::decompress(blob);
        REQUIRE(back.has_value());
        CHECK(back.value().empty());
    }
}
