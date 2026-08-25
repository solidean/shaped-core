#include "stream-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/streams/growing_stream.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cc_stream_test;

// Both adapters convert to their natural seekable write stream and to the plain narrowing of it, and never to a read stream.
static_assert(std::is_convertible_v<cc::vector_write_stream_adapter, cc::seekable_write_stream>);
static_assert(std::is_convertible_v<cc::vector_write_stream_adapter, cc::write_stream>);
static_assert(std::is_convertible_v<cc::string_write_stream_adapter, cc::seekable_write_stream>);
static_assert(!std::is_convertible_v<cc::string_write_stream_adapter, cc::read_stream>);

namespace
{
cc::span<byte const> as_bytes(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
}
} // namespace

TEST("growing_stream - grows from empty")
{
    auto adapter = cc::string_write_stream_adapter();
    {
        auto s = adapter.stream();
        REQUIRE(s.write(as_bytes("hello")).has_value());
        REQUIRE(s.flush().has_value());
    }
    CHECK(adapter.take() == "hello");
}

TEST("growing_stream - many small writes across flushes")
{
    // far past the initial window, so the buffer reallocates repeatedly and every pointer must be recomputed
    constexpr isize n = 10000;

    auto adapter = cc::vector_write_stream_adapter();
    auto s = adapter.stream();
    for (auto i = isize(0); i < n; ++i)
    {
        auto const v = byte(i % 251);
        REQUIRE(s.write(cc::span<byte const>(&v, 1)).has_value());
    }
    REQUIRE(s.flush().has_value());

    auto const data = adapter.take();
    REQUIRE(data.size() == n);
    auto ok = true;
    for (auto i = isize(0); i < n; ++i)
        ok = ok && data[i] == byte(i % 251);
    CHECK(ok);
}

TEST("growing_stream - one large write past the window")
{
    auto const big = cc::string::create_filled(5000, 'x');

    auto adapter = cc::string_write_stream_adapter();
    auto s = adapter.stream();
    REQUIRE(s.write(as_bytes(big)).has_value());
    REQUIRE(s.write(as_bytes("tail")).has_value());
    REQUIRE(s.flush().has_value());

    auto const text = adapter.take();
    CHECK(text.size() == 5004);
    CHECK(cc::string_view(text).subview({.offset = 5000, .size = 4}) == "tail");
}

TEST("growing_stream - appends after initial content")
{
    auto adapter = cc::string_write_stream_adapter(cc::string("head:"));
    auto s = adapter.stream();
    CHECK(s.position().value() == 5);
    REQUIRE(s.write(as_bytes("tail")).has_value());
    REQUIRE(s.flush().has_value());
    CHECK(adapter.take() == "head:tail");
}

TEST("growing_stream - seek back and patch")
{
    auto adapter = cc::string_write_stream_adapter();
    auto s = adapter.stream();

    REQUIRE(s.write(as_bytes("0000 payload")).has_value());
    CHECK(s.size().value() == 12); // pending bytes count towards the size before they are committed

    // patch the placeholder written before the payload was known
    REQUIRE(s.seek_to(0).value() == 0);
    REQUIRE(s.write(as_bytes("1234")).has_value());
    REQUIRE(s.seek_from_end(0).value() == 12);
    REQUIRE(s.write(as_bytes("!")).has_value());
    REQUIRE(s.flush().has_value());

    CHECK(adapter.take() == "1234 payload!");
}

TEST("growing_stream - write_pod round trip")
{
    auto adapter = cc::vector_write_stream_adapter();
    auto s = adapter.stream();
    REQUIRE(s.write_pod(u32(0xDEADBEEF)).has_value());
    REQUIRE(s.write_pod(u32(7)).has_value());
    REQUIRE(s.flush().has_value());

    auto const data = adapter.take();
    REQUIRE(data.size() == 8);
    CHECK(*reinterpret_cast<u32 const*>(data.data()) == 0xDEADBEEF);
    CHECK(*reinterpret_cast<u32 const*>(data.data() + 4) == 7u);
}

TEST("growing_stream - seek out of range errors")
{
    auto adapter = cc::vector_write_stream_adapter();
    auto s = adapter.stream();
    REQUIRE(s.write(as_bytes("abc")).has_value());
    CHECK(!s.seek_to(4).has_value());
    CHECK(!s.seek_to(-1).has_value());
}

TEST("growing_stream - take without a flush sees only committed bytes")
{
    auto adapter = cc::vector_write_stream_adapter();
    {
        auto s = adapter.stream();
        REQUIRE(s.write(as_bytes("pending")).has_value());
        // no flush: the bytes sit in the spare capacity and are not part of the container yet
    }
    CHECK(adapter.take().empty());
}
