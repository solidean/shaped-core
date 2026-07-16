#include "stream-test-types.hh"

#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <nexus/test.hh>

using namespace cc_stream_test;

// --- streams don't convert to each other; adapters convert straight to any narrower type -------------------

// A stream, once made, is its type: there is NO stream-to-stream conversion (see span_stream-test for the
// adapter-side narrowing conversions, which is how you get a narrower type).
static_assert(!std::is_constructible_v<cc::read_stream, cc::seekable_read_stream&&>);
static_assert(!std::is_constructible_v<cc::read_stream, cc::seekable_read_write_stream&&>);
static_assert(!std::is_constructible_v<cc::read_stream, cc::write_stream&&>);

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

TEST("stream - move invalidates the source")
{
    cc::byte data[3] = {b(7), b(8), b(9)};
    cc::span_read_stream_adapter adapter{cc::span<cc::byte const>(data)};

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
    cc::span_read_stream_adapter adapter{cc::span<cc::byte const>(data)};

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
