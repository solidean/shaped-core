#include "stream-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/streams/span_stream.hh>
#include <nexus/test.hh>

using namespace cc_stream_test;

// An adapter converts implicitly to its natural seekable stream AND to any legal narrowing of it — crucially
// to the plain (non-seekable) form, so it drops straight into a function that expects only a read/write stream.
static_assert(std::is_convertible_v<cc::span_read_stream_adapter, cc::seekable_read_stream>);
static_assert(std::is_convertible_v<cc::span_read_stream_adapter, cc::read_stream>);
static_assert(std::is_convertible_v<cc::span_write_stream_adapter, cc::write_stream>);
static_assert(std::is_convertible_v<cc::span_read_write_stream_adapter, cc::read_write_stream>);
static_assert(std::is_convertible_v<cc::span_read_write_stream_adapter, cc::read_stream>);  // drops seekable + write
static_assert(std::is_convertible_v<cc::span_read_write_stream_adapter, cc::write_stream>); // drops seekable + read
// but never across leaf capabilities (read-only adapter is not a write stream, and vice versa)
static_assert(!std::is_convertible_v<cc::span_read_stream_adapter, cc::write_stream>);
static_assert(!std::is_convertible_v<cc::span_write_stream_adapter, cc::read_stream>);

TEST("span_stream - read round-trip and seeking")
{
    cc::byte const data[5] = {b(10), b(11), b(12), b(13), b(14)};
    cc::span_read_stream_adapter adapter{cc::span<cc::byte const>(data)};
    cc::seekable_read_stream s = adapter.stream();

    // whole span is available up front (unbuffered)
    CHECK(bytes_equal(s.ready_bytes(), cc::span<cc::byte const>(data)));

    auto sz = s.size();
    REQUIRE(sz.has_value());
    CHECK(sz.value() == 5);

    // absolute seek
    REQUIRE(s.seek_to(2).value() == 2);
    CHECK(s.position().value() == 2);
    CHECK(s.remaining_bytes().value() == 3);
    CHECK(bytes_equal(s.ready_bytes(), cc::span<cc::byte const>({b(12), b(13), b(14)})));

    // relative seek
    REQUIRE(s.skip(1).value() == 3);
    CHECK(s.position().value() == 3);

    // seek from the end
    REQUIRE(s.seek_from_end(-1).value() == 4);
    cc::byte last[1] = {b(0)};
    CHECK(s.read(last).value() == 1);
    CHECK(last[0] == b(14));
    CHECK(s.at_end().value());
}

TEST("span_stream - read helpers")
{
    cc::byte const data[4] = {b(1), b(2), b(3), b(4)};
    cc::span_read_stream_adapter adapter{cc::span<cc::byte const>(data)};
    cc::seekable_read_stream s = adapter.stream();

    cc::vector<cc::byte> out = cc::vector<cc::byte>::create_defaulted(4);
    auto n = s.read(out);
    REQUIRE(n.has_value());
    CHECK(n.value() == 4);
    CHECK(bytes_equal(out, cc::span<cc::byte const>(data)));

    // exhausted
    CHECK(s.at_end().value());
    CHECK(s.read(out).value() == 0);
}

TEST("span_stream - empty span is immediately at end")
{
    cc::span_read_stream_adapter adapter{cc::span<cc::byte const>()};
    cc::seekable_read_stream s = adapter.stream();

    CHECK(s.at_end().value());
    CHECK(s.size().value() == 0);

    cc::vector<cc::byte> out = cc::vector<cc::byte>::create_defaulted(4);
    CHECK(s.read(out).value() == 0);
}

TEST("span_stream - write into a mutable span")
{
    cc::byte buf[4] = {b(0), b(0), b(0), b(0)};
    cc::span_write_stream_adapter adapter(buf);
    cc::seekable_write_stream s = adapter.stream();

    REQUIRE(s.write(cc::span<cc::byte const>({b(1), b(2), b(3), b(4)})).has_value());
    // bytes land directly in the destination span (unbuffered)
    cc::byte const expected[4] = {b(1), b(2), b(3), b(4)};
    CHECK(bytes_equal(cc::span<cc::byte const>(buf), cc::span<cc::byte const>(expected)));
}

TEST("span_stream - bounded write errors when the span is full")
{
    cc::byte buf[4] = {b(0), b(0), b(0), b(0)};
    cc::span_write_stream_adapter adapter(buf);
    cc::seekable_write_stream s = adapter.stream();

    // one byte more than the span holds
    auto r = s.write(cc::span<cc::byte const>({b(1), b(2), b(3), b(4), b(5)}));
    CHECK(r.has_error());
}

TEST("span_stream - read_write shares a single cursor")
{
    cc::byte buf[4] = {b(1), b(2), b(3), b(4)};
    cc::span_read_write_stream_adapter adapter(buf);
    cc::seekable_read_write_stream s = adapter.stream();

    // read the first two, overwrite the last two
    cc::vector<cc::byte> head = cc::vector<cc::byte>::create_defaulted(2);
    REQUIRE(s.read(head).value() == 2);
    CHECK(bytes_equal(head, cc::span<cc::byte const>({b(1), b(2)})));
    REQUIRE(s.write(cc::span<cc::byte const>({b(9), b(9)})).has_value());

    // rewind and read everything back
    REQUIRE(s.seek_to(0).value() == 0);
    cc::vector<cc::byte> all = cc::vector<cc::byte>::create_defaulted(4);
    REQUIRE(s.read(all).value() == 4);
    CHECK(bytes_equal(all, cc::span<cc::byte const>({b(1), b(2), b(9), b(9)})));
}
