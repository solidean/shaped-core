#include "stream-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

#include <filesystem>
#include <string>
#include <type_traits>

using namespace cc_stream_test;

// File adapters convert implicitly to their natural seekable stream and to any legal narrowing (incl. the
// non-seekable form), so they drop into a function taking a plain read/write/read_write stream.
static_assert(std::is_convertible_v<cc::file_read_stream_adapter, cc::read_stream>);
static_assert(std::is_convertible_v<cc::file_write_stream_adapter, cc::write_stream>);
static_assert(std::is_convertible_v<cc::file_read_write_stream_adapter, cc::read_write_stream>);
static_assert(std::is_convertible_v<cc::file_read_write_stream_adapter, cc::read_stream>);
static_assert(std::is_convertible_v<cc::file_read_write_stream_adapter, cc::write_stream>);

namespace
{
// A unique writable path in the OS temp dir, removed when the guard goes out of scope.
struct temp_file
{
    std::string path;

    explicit temp_file(char const* name) : path((std::filesystem::temp_directory_path() / name).string())
    {
        this->remove();
    }
    ~temp_file() { this->remove(); }
    temp_file(temp_file const&) = delete;
    temp_file& operator=(temp_file const&) = delete;

    [[nodiscard]] cc::string_view view() const { return cc::string_view(path.c_str()); }
    void remove() const
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

cc::vector<cc::byte> make_pattern(cc::isize n)
{
    auto v = cc::vector<cc::byte>::create_defaulted(n);
    for (cc::isize i = 0; i < n; ++i)
        v[i] = cc::byte((i * 7 + 3) & 0xFF);
    return v;
}

// n bytes starting at `start` (mod 256) — distinct, easy-to-compare content.
cc::vector<cc::byte> make_bytes(cc::isize n, int start)
{
    auto v = cc::vector<cc::byte>::create_defaulted(n);
    for (cc::isize i = 0; i < n; ++i)
        v[i] = cc::byte((start + int(i)) & 0xFF);
    return v;
}

// Write `bytes` to `path` through a file_write_stream_adapter, flushing and closing before returning.
void write_file(cc::string_view path, cc::span<cc::byte const> bytes)
{
    auto wpr = cc::file_write_stream_adapter::create(path);
    REQUIRE(wpr.has_value());
    auto ws = wpr.value().stream();
    REQUIRE(ws.write(bytes).has_value());
    REQUIRE(ws.flush().has_value());
    // ws then wpr destroyed here -> file handle closed
}

// Read the whole file back into a vector (via file_read_stream_adapter).
cc::vector<cc::byte> read_whole(cc::string_view path)
{
    auto rpr = cc::file_read_stream_adapter::open(path);
    REQUIRE(rpr.has_value());
    auto rs = rpr.value().stream();
    auto const n = rs.size().value();
    auto out = cc::vector<cc::byte>::create_defaulted(n);
    REQUIRE(rs.read_exact(out).has_value());
    return out;
}
} // namespace

TEST("file_stream - write then read round-trip across many buffers")
{
    temp_file tf("cc-stream-roundtrip.tmp");
    auto const pattern = make_pattern(10000); // > 2 * 4 KiB buffer

    write_file(tf.view(), pattern);

    auto rpr = cc::file_read_stream_adapter::open(tf.view());
    REQUIRE(rpr.has_value());
    auto rs = rpr.value().stream();

    CHECK(rs.size().value() == 10000);

    auto out = cc::vector<cc::byte>::create_defaulted(10000);
    REQUIRE(rs.read_exact(out).has_value());
    CHECK(bytes_equal(out, pattern));

    // fully drained now; a repeated flush at EOF stays at end and keeps the position
    CHECK(rs.at_end().value());
    CHECK(rs.flush().value() == 10000);
    CHECK(rs.at_end().value());
}

TEST("file_stream - write buffer exactly full")
{
    temp_file tf("cc-stream-exact.tmp");
    auto const pattern = make_pattern(cc::file_write_stream_adapter::k_buffer_size); // exactly one buffer

    write_file(tf.view(), pattern);

    auto rpr = cc::file_read_stream_adapter::open(tf.view());
    REQUIRE(rpr.has_value());
    auto rs = rpr.value().stream();
    CHECK(rs.size().value() == cc::file_write_stream_adapter::k_buffer_size);

    auto out = cc::vector<cc::byte>::create_defaulted(pattern.size());
    REQUIRE(rs.read_exact(out).has_value());
    CHECK(bytes_equal(out, pattern));
}

TEST("file_stream - zero-byte file is immediately at end")
{
    temp_file tf("cc-stream-empty.tmp");
    write_file(tf.view(), cc::span<cc::byte const>());

    auto rpr = cc::file_read_stream_adapter::open(tf.view());
    REQUIRE(rpr.has_value());
    auto rs = rpr.value().stream();

    CHECK(rs.size().value() == 0);
    CHECK(rs.at_end().value());

    auto out = cc::vector<cc::byte>::create_defaulted(16);
    CHECK(rs.read(out).value() == 0);
}

TEST("file_stream - partial consume then refill preserves the leftover")
{
    temp_file tf("cc-stream-leftover.tmp");
    auto const pattern = make_pattern(6000);
    write_file(tf.view(), pattern);

    auto rpr = cc::file_read_stream_adapter::open(tf.view());
    REQUIRE(rpr.has_value());
    auto rs = rpr.value().stream();

    // fill the buffer, then consume only a little of it via the low-level window
    REQUIRE(rs.flush().has_value());
    CHECK(rs.ready_bytes().size() == cc::file_read_stream_adapter::k_buffer_size);
    CHECK(bytes_equal(rs.ready_bytes().first_n(10), cc::span<cc::byte const>(pattern).first_n(10)));
    rs.consume(10);

    // reading the rest crosses the buffer boundary; the leftover from the first fill must be preserved
    auto rest = cc::vector<cc::byte>::create_defaulted(6000 - 10);
    REQUIRE(rs.read_exact(rest).has_value());
    CHECK(bytes_equal(rest, cc::span<cc::byte const>(pattern).subspan(cc::offset_size{.offset = 10, .size = 6000 - 10})));
}

TEST("file_stream - seek past end reads as end-of-file")
{
    temp_file tf("cc-stream-seek.tmp");
    auto const pattern = make_pattern(100);
    write_file(tf.view(), pattern);

    auto rpr = cc::file_read_stream_adapter::open(tf.view());
    REQUIRE(rpr.has_value());
    auto rs = rpr.value().stream();

    // absolute seek within the file
    REQUIRE(rs.seek_to(90).value() == 90);
    auto tail = cc::vector<cc::byte>::create_defaulted(10);
    CHECK(rs.read(tail).value() == 10);
    CHECK(bytes_equal(tail, cc::span<cc::byte const>(pattern).subspan(cc::offset_size{.offset = 90, .size = 10})));

    // seeking past the end is allowed; the next read yields nothing
    REQUIRE(rs.seek_to(1000).value() == 1000);
    CHECK(rs.at_end().value());
    auto out = cc::vector<cc::byte>::create_defaulted(4);
    CHECK(rs.read(out).value() == 0);
}

TEST("file_stream - size reflects buffered-but-unflushed writes")
{
    temp_file tf("cc-stream-size.tmp");

    auto wpr = cc::file_write_stream_adapter::create(tf.view());
    REQUIRE(wpr.has_value());
    auto ws = wpr.value().stream();

    REQUIRE(ws.write(make_pattern(100)).has_value()); // buffered, not yet flushed
    CHECK(ws.size().value() == 100);
    CHECK(ws.position().value() == 100);

    REQUIRE(ws.flush().has_value());
}

TEST("file_stream - write open overwrites without truncating")
{
    temp_file tf("cc-stream-open-overwrite.tmp");
    auto const original = make_bytes(100, 0);
    write_file(tf.view(), original);

    auto const head = make_bytes(10, 200);
    {
        auto wp = cc::file_write_stream_adapter::open(tf.view());
        REQUIRE(wp.has_value());
        auto ws = wp.value().stream();
        REQUIRE(ws.write(head).has_value()); // overwrite the first 10 bytes; do NOT truncate
        REQUIRE(ws.flush().has_value());
    }

    auto const got = read_whole(tf.view());
    REQUIRE(got.size() == 100); // still the original length
    CHECK(bytes_equal(cc::span<cc::byte const>(got).first_n(10), head));
    CHECK(bytes_equal(cc::span<cc::byte const>(got).subspan(cc::offset_size{.offset = 10, .size = 90}),
                      cc::span<cc::byte const>(original).subspan(cc::offset_size{.offset = 10, .size = 90})));
}

TEST("file_stream - write append grows an existing file")
{
    temp_file tf("cc-stream-append.tmp");
    auto const original = make_bytes(50, 0);
    write_file(tf.view(), original);

    auto const extra = make_bytes(20, 100);
    {
        auto ap = cc::file_write_stream_adapter::append(tf.view());
        REQUIRE(ap.has_value());
        auto ws = ap.value().stream();
        REQUIRE(ws.write(extra).has_value()); // starts at EOF -> grows
        REQUIRE(ws.flush().has_value());
    }

    auto const got = read_whole(tf.view());
    REQUIRE(got.size() == 70);
    CHECK(bytes_equal(cc::span<cc::byte const>(got).first_n(50), original));
    CHECK(bytes_equal(cc::span<cc::byte const>(got).subspan(cc::offset_size{.offset = 50, .size = 20}), extra));
}

TEST("file_stream - read_write can read the whole file")
{
    temp_file tf("cc-stream-rw-read.tmp");
    auto const original = make_bytes(300, 0); // spans multiple buffers
    write_file(tf.view(), original);

    auto rw = cc::file_read_write_stream_adapter::open(tf.view());
    REQUIRE(rw.has_value());
    auto s = rw.value().stream();
    CHECK(s.size().value() == 300);
    auto out = cc::vector<cc::byte>::create_defaulted(300);
    REQUIRE(s.read_exact(out).has_value());
    CHECK(bytes_equal(out, original));
}

TEST("file_stream - read_write overwrites in place")
{
    temp_file tf("cc-stream-rw-overwrite.tmp");
    auto const original = make_bytes(200, 0);
    write_file(tf.view(), original);

    auto const marker = make_bytes(10, 240);
    {
        auto rw = cc::file_read_write_stream_adapter::open(tf.view());
        REQUIRE(rw.has_value());
        auto s = rw.value().stream();
        REQUIRE(s.seek_to(50).value() == 50);
        REQUIRE(s.write(marker).has_value()); // overwrite bytes 50..59
        REQUIRE(s.flush().has_value());
    }

    auto expected = original; // deep copy, then patch the overwritten range
    for (cc::isize i = 0; i < marker.size(); ++i)
        expected[50 + i] = marker[i];

    auto const got = read_whole(tf.view());
    REQUIRE(got.size() == 200); // length unchanged
    CHECK(bytes_equal(got, expected));
}

TEST("file_stream - read_write grows while writing past the end")
{
    temp_file tf("cc-stream-rw-grow.tmp");
    auto const original = make_bytes(100, 0);
    write_file(tf.view(), original);

    auto const tail = make_bytes(30, 128);
    {
        auto rw = cc::file_read_write_stream_adapter::open(tf.view());
        REQUIRE(rw.has_value());
        auto s = rw.value().stream();
        REQUIRE(s.seek_to(90).value() == 90);
        REQUIRE(s.write(tail).has_value()); // overwrites 90..99, then extends 100..119
        REQUIRE(s.flush().has_value());
    }

    auto const got = read_whole(tf.view());
    REQUIRE(got.size() == 120); // grew by 20
    CHECK(bytes_equal(cc::span<cc::byte const>(got).first_n(90), cc::span<cc::byte const>(original).first_n(90)));
    CHECK(bytes_equal(cc::span<cc::byte const>(got).subspan(cc::offset_size{.offset = 90, .size = 30}), tail));
}

TEST("file_stream - read_write appends at EOF via seek-to-end then write")
{
    // the fresh-append case: seek to EOF (window empty for reads) then write with nothing buffered. The
    // separate write-capacity end makes free_bytes() non-empty at EOF, so the append just works.
    temp_file tf("cc-stream-rw-append.tmp");
    auto const original = make_bytes(40, 0);
    write_file(tf.view(), original);

    auto const extra = make_bytes(15, 200);
    {
        auto rw = cc::file_read_write_stream_adapter::open(tf.view());
        REQUIRE(rw.has_value());
        auto s = rw.value().stream();
        REQUIRE(s.seek_from_end(0).value() == 40); // fresh window at EOF, no read data buffered
        REQUIRE(s.write(extra).has_value());       // append
        REQUIRE(s.flush().has_value());
    }

    auto const got = read_whole(tf.view());
    REQUIRE(got.size() == 55);
    CHECK(bytes_equal(cc::span<cc::byte const>(got).first_n(40), original));
    CHECK(bytes_equal(cc::span<cc::byte const>(got).subspan(cc::offset_size{.offset = 40, .size = 15}), extra));
}
