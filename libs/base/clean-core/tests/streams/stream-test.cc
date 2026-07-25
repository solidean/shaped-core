#include "stream-test-types.hh"

#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <nexus/test.hh>

using namespace cc_stream_test;

// --- conversions only ever narrow, and only from an rvalue -------------------------------------------------

// Every legal narrowing: drop seekable, and read_write -> read or write.
static_assert(std::is_constructible_v<cc::read_stream, cc::seekable_read_stream&&>);
static_assert(std::is_constructible_v<cc::write_stream, cc::seekable_write_stream&&>);
static_assert(std::is_constructible_v<cc::read_stream, cc::read_write_stream&&>);
static_assert(std::is_constructible_v<cc::write_stream, cc::read_write_stream&&>);
static_assert(std::is_constructible_v<cc::read_write_stream, cc::seekable_read_write_stream&&>);
static_assert(std::is_constructible_v<cc::seekable_read_stream, cc::seekable_read_write_stream&&>);
static_assert(std::is_constructible_v<cc::seekable_write_stream, cc::seekable_read_write_stream&&>);
static_assert(std::is_constructible_v<cc::read_stream, cc::seekable_read_write_stream&&>);
static_assert(std::is_constructible_v<cc::write_stream, cc::seekable_read_write_stream&&>);

// read and write are leaf capabilities: they never cross, in either direction.
static_assert(!std::is_constructible_v<cc::read_stream, cc::write_stream&&>);
static_assert(!std::is_constructible_v<cc::write_stream, cc::read_stream&&>);

// Widening is not a conversion — a capability the source never had cannot appear.
static_assert(!std::is_constructible_v<cc::read_write_stream, cc::read_stream&&>);
static_assert(!std::is_constructible_v<cc::seekable_read_stream, cc::read_stream&&>);

// An LVALUE never converts: narrowing consumes the source, so a second live view onto one adapter
// (whose flush is stateful) can never exist.
static_assert(!std::is_constructible_v<cc::read_stream, cc::seekable_read_stream&>);
static_assert(!std::is_constructible_v<cc::write_stream, cc::read_write_stream&>);

// streams are move-only
static_assert(!std::is_copy_constructible_v<cc::read_stream>);
static_assert(std::is_move_constructible_v<cc::read_stream>);

TEST("stream - adapter converts directly to a narrower stream")
{
    cc::byte data[4] = {b(1), b(2), b(3), b(4)};
    cc::span_read_write_stream_adapter adapter(data);

    // the read_write adapter drops straight to a plain (non-seekable) read_stream
    cc::read_stream r = adapter;
    CHECK(r.is_valid());
    cc::vector<cc::byte> out = cc::vector<cc::byte>::create_defaulted(4);
    REQUIRE(r.read(out).value() == 4);
    CHECK(bytes_equal(out, cc::span<cc::byte const>(data)));
}

TEST("stream - narrowing consumes the source and keeps reading through the same adapter")
{
    cc::byte data[4] = {b(1), b(2), b(3), b(4)};
    auto adapter = cc::span_read_stream_adapter(cc::span<cc::byte const>(data));

    cc::seekable_read_stream seekable = adapter.stream();
    REQUIRE(seekable.read_pod<cc::byte>().value() == b(1)); // consume one byte BEFORE narrowing

    cc::read_stream plain = cc::move(seekable);
    CHECK(!seekable.is_valid());
    CHECK_ASSERTS(seekable.flush()); // the consumed source is unusable, not a second view

    // the window carried over intact: reading resumes where the seekable stream left off
    cc::vector<cc::byte> out = cc::vector<cc::byte>::create_defaulted(3);
    REQUIRE(plain.read(out).value() == 3);
    cc::byte const rest[3] = {b(2), b(3), b(4)};
    CHECK(bytes_equal(out, cc::span<cc::byte const>(rest)));
}

TEST("stream - narrowing read_write to write keeps the write bound, not the read one")
{
    // `end` is the read boundary on a read_write stream but the write bound on a write stream, so this
    // narrowing has to carry over write_end. Taking `end` would silently shrink the sink to the readable part.
    auto adapter = mock_split_bounds_read_write_adapter(/*readable*/ cc::isize(4));

    cc::read_write_stream rw = adapter.stream();
    REQUIRE(rw.flush().has_value());
    REQUIRE(rw.ready_bytes().size() == 4);     // read boundary
    REQUIRE(rw.writable_bytes().size() == 16); // write capacity — deliberately wider

    cc::write_stream w = cc::move(rw);
    CHECK(!rw.is_valid());
    CHECK(w.writable_bytes().size() == 16);
}

TEST("stream - narrowing away write capability with pending writes asserts")
{
    // The pending bytes sit in the source's buffer and drain only through a write-capable flush; a read_stream
    // could never push them out, so losing them silently is the one thing this must not do.
    auto adapter = mock_split_bounds_read_write_adapter(/*readable*/ cc::isize(0));

    cc::read_write_stream rw = adapter.stream();
    REQUIRE(rw.flush().has_value());
    REQUIRE(rw.write(cc::span<cc::byte const>({b(9)})).has_value());

    CHECK_ASSERTS(cc::read_stream(cc::move(rw)));
}

TEST("stream - move invalidates the source")
{
    cc::byte data[3] = {b(7), b(8), b(9)};
    auto adapter = cc::span_read_stream_adapter(cc::span<cc::byte const>(data));

    cc::seekable_read_stream a = adapter.stream();
    CHECK(a.is_valid());

    cc::seekable_read_stream moved = cc::move(a);
    CHECK(moved.is_valid());
    CHECK(!a.is_valid());
    CHECK_ASSERTS(a.flush()); // using the moved-from stream asserts
}

TEST("stream - adapter converts implicitly to a stream")
{
    cc::byte data[3] = {b(7), b(8), b(9)};

    // pass an adapter straight into something expecting a stream
    auto sum_bytes = [](cc::read_stream s) -> cc::i64
    {
        cc::i64 total = 0;
        for (cc::byte v : s.ready_bytes())
            total += cc::i64(v);
        return total;
    };

    CHECK(sum_bytes(cc::span_read_stream_adapter(cc::span<cc::byte const>(data))) == 24);
}

TEST("stream - try_as_seekable fails on a non-seekable source, leaving it valid")
{
    cc::byte data[8] = {b(0), b(1), b(2), b(3), b(4), b(5), b(6), b(7)};
    mock_pipe_read_stream_adapter adapter(cc::span<cc::byte const>(data), /*chunk*/ 3);

    cc::read_stream s = adapter.stream();

    auto upgraded = cc::move(s).try_as_seekable();
    CHECK(!upgraded.has_value());
    CHECK(s.is_valid()); // dry probe failed -> original stays usable

    // the dry probe must not have disturbed the buffer: a fresh read still yields all the data in order
    cc::vector<cc::byte> out = cc::vector<cc::byte>::create_defaulted(8);
    auto n = s.read(out);
    REQUIRE(n.has_value());
    CHECK(n.value() == 8);
    CHECK(bytes_equal(out, cc::span<cc::byte const>(data)));
}

TEST("stream - try_as_seekable succeeds on a seekable source")
{
    cc::byte data[4] = {b(1), b(2), b(3), b(4)};
    auto adapter = cc::span_read_stream_adapter(cc::span<cc::byte const>(data));

    // erase the seekable capability (adapter -> plain read_stream), then recover it via the dry probe
    cc::read_stream s = adapter;
    auto upgraded = cc::move(s).try_as_seekable();
    REQUIRE(upgraded.has_value());
    CHECK(!s.is_valid());

    auto& seekable = upgraded.value();
    auto sz = seekable.size();
    REQUIRE(sz.has_value());
    CHECK(sz.value() == 4);
}

TEST("stream - first_write is set on write and reset after each flush")
{
    recording_write_stream_adapter adapter;
    cc::write_stream s = adapter.stream();

    REQUIRE(s.write(cc::span<cc::byte const>({b(1), b(2)})).has_value());
    REQUIRE(s.flush().has_value());
    REQUIRE(s.write(cc::span<cc::byte const>({b(3), b(4), b(5)})).has_value());
    REQUIRE(s.flush().has_value());

    // an idempotent flush with nothing pending must not record anything
    REQUIRE(s.flush().has_value());

    cc::byte const expected[5] = {b(1), b(2), b(3), b(4), b(5)};
    CHECK(bytes_equal(adapter.written(), cc::span<cc::byte const>(expected)));
    CHECK(adapter.flushes_with_pending() == 2); // exactly the two flushes that carried data
}

TEST("stream - read_all on a seekable source (precise, single allocation)")
{
    cc::byte data[5] = {b(10), b(11), b(12), b(13), b(14)};
    auto adapter = cc::span_read_stream_adapter(cc::span<cc::byte const>(data));
    cc::read_stream s = adapter; // narrowed to non-seekable, but the adapter still reports position

    auto all = s.read_all();
    REQUIRE(all.has_value());
    CHECK(bytes_equal(all.value(), cc::span<cc::byte const>(data)));

    // consumed to end: a second read_all yields nothing
    auto again = s.read_all();
    REQUIRE(again.has_value());
    CHECK(again.value().empty());
}

TEST("stream - read_all on a non-seekable pipe (grows across chunks)")
{
    cc::byte source[70];
    auto const source_size = cc::isize(sizeof(source));
    for (auto i = cc::isize(0); i < source_size; ++i)
        source[i] = b(int(i));

    // chunk 16 < 64-byte buffer < 70-byte total, so the read spans several refills, and the pipe reports no position -> read_all cannot size up front and must grow.
    auto adapter = mock_pipe_read_stream_adapter(cc::span<cc::byte const>(source), cc::isize(16));
    cc::read_stream s = adapter.stream();

    auto all = s.read_all();
    REQUIRE(all.has_value());
    CHECK(bytes_equal(all.value(), cc::span<cc::byte const>(source)));
}

TEST("stream - read_all on an empty source yields an empty buffer")
{
    auto adapter = cc::span_read_stream_adapter(cc::span<cc::byte const>());
    cc::read_stream s = adapter;

    auto all = s.read_all();
    REQUIRE(all.has_value());
    CHECK(all.value().empty());
}
