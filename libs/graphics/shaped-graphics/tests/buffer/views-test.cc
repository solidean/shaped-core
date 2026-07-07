#include <nexus/test.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/views.hh>

#include <memory>

// Views are pure value types over a buffer, so they need no GPU backend — a minimal concrete
// buffer subclass (shape metadata only) is enough to exercise every factory + the erased raw_view.
// Buffers must be held via shared_ptr here: the factories call shared_from_this().

namespace
{
struct test_buffer final : sg::raw_buffer
{
    test_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) : sg::raw_buffer(size_in_bytes, usage) {}
};

// A 16-byte element (structured views require sizeof % 4 == 0).
struct particle
{
    sg::u32 a, b, c, d;
};

// A 2-byte struct — not a valid view element (not `byte`, not a multiple of 4).
struct two_bytes
{
    sg::u8 a, b;
};

std::shared_ptr<test_buffer> make_buffer(sg::isize size, sg::buffer_usage usage)
{
    return std::make_shared<test_buffer>(size, usage);
}
} // namespace

static_assert(sg::view_element<sg::byte>);
static_assert(sg::view_element<sg::u32>);
static_assert(sg::view_element<particle>);
static_assert(!sg::view_element<two_bytes>);
static_assert(!sg::view_element<sg::u16>); // 2 bytes

// uniform blocks are stricter: 16-byte aligned size, not `byte`.
static_assert(sg::uniform_element<particle>);  // 16 bytes
static_assert(!sg::uniform_element<sg::byte>); // 1 byte — a uniform block of raw bytes is meaningless
static_assert(!sg::uniform_element<sg::u32>);  // 4 bytes — not a multiple of 16

TEST("sg views - uniform view")
{
    auto const buf = make_buffer(1024, sg::buffer_usage::uniform_buffer);

    auto const v = buf->as_uniform_buffer<particle>();
    CHECK(v.buffer == buf);
    CHECK(v.offset_in_bytes == 0);
    CHECK(v.size_in_bytes == sizeof(particle));
    CHECK(sg::uniform_view<particle>::access == sg::view_class::uniform);

    auto const raw = v.to_raw();
    CHECK(raw.access == sg::view_class::uniform);
    CHECK(raw.shape == sg::view_shape::uniform_block);
    CHECK(raw.buffer == buf);
    CHECK(raw.size_in_bytes == sizeof(particle));

    // Optional byte offset (must be 256-byte aligned): select one block of a UBO array.
    auto const at = buf->as_uniform_buffer<particle>(256);
    CHECK(at.offset_in_bytes == 256);
    CHECK(at.size_in_bytes == sizeof(particle));

    // A misaligned offset asserts (not a multiple of 256).
    CHECK_ASSERTS(buf->as_uniform_buffer<particle>(16));

    // A block that does not fit past the (aligned) offset asserts.
    CHECK_ASSERTS(buf->as_uniform_buffer<particle>(1024));
}

TEST("sg views - readonly structured view")
{
    auto const buf = make_buffer(sg::isize(sizeof(particle)) * 10, sg::buffer_usage::readonly_buffer);

    // Whole buffer: element_count == size / sizeof(T).
    auto const whole = buf->as_readonly_buffer<particle>();
    CHECK(whole.offset_in_bytes == 0);
    CHECK(whole.element_count == 10);

    auto const raw = whole.to_raw();
    CHECK(raw.access == sg::view_class::readonly);
    CHECK(raw.shape == sg::view_shape::structured);
    CHECK(raw.element_count == 10);
    CHECK(raw.stride_in_bytes == sizeof(particle));

    // Sub-range in elements: offset is scaled by the stride.
    auto const sub = buf->as_readonly_buffer<sg::u32>({.offset = 2, .size = 3});
    CHECK(sub.offset_in_bytes == 2 * sizeof(sg::u32));
    CHECK(sub.element_count == 3);
    CHECK(sub.to_raw().stride_in_bytes == sizeof(sg::u32));
}

TEST("sg views - readwrite structured view")
{
    auto const buf = make_buffer(sg::isize(sizeof(particle)) * 4, sg::buffer_usage::readwrite_buffer);

    auto const v = buf->as_readwrite_buffer<particle>();
    CHECK(v.element_count == 4);

    auto const raw = v.to_raw();
    CHECK(raw.access == sg::view_class::readwrite);
    CHECK(raw.shape == sg::view_shape::structured);
    CHECK(raw.stride_in_bytes == sizeof(particle));
}

TEST("sg views - raw byte views")
{
    auto const buf = make_buffer(256, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer);

    auto const ro = buf->as_raw_readonly();
    auto const ro_raw = ro.to_raw();
    CHECK(ro_raw.access == sg::view_class::readonly);
    CHECK(ro_raw.shape == sg::view_shape::raw);
    CHECK(ro_raw.size_in_bytes == 256);
    CHECK(ro_raw.stride_in_bytes == 0);

    auto const rw = buf->as_raw_readwrite();
    auto const rw_raw = rw.to_raw();
    CHECK(rw_raw.access == sg::view_class::readwrite);
    CHECK(rw_raw.shape == sg::view_shape::raw);
    CHECK(rw_raw.size_in_bytes == 256);

    // Subrange, in bytes.
    auto const sub = buf->as_raw_readonly({.offset = 64, .size = 128});
    CHECK(sub.offset_in_bytes == 64);
    auto const sub_raw = sub.to_raw();
    CHECK(sub_raw.shape == sg::view_shape::raw);
    CHECK(sub_raw.size_in_bytes == 128);
}

TEST("sg views - implicit conversion to raw_view")
{
    auto const buf = make_buffer(64, sg::buffer_usage::readonly_buffer);

    // The typed view converts implicitly to the erased form a backend consumes.
    sg::raw_view const rv = buf->as_readonly_buffer<sg::u32>();
    CHECK(rv.shape == sg::view_shape::structured);
    CHECK(rv.element_count == 16);
}

TEST("sg views - empty buffer yields empty view")
{
    // Size 0 is a valid empty buffer; a whole-buffer view over it is a zero-element view.
    auto const buf = make_buffer(0, sg::buffer_usage::readonly_buffer);

    auto const v = buf->as_readonly_buffer<particle>();
    CHECK(v.element_count == 0);
}

TEST("sg views - misuse asserts")
{
    // Wrong usage: a readonly view over a buffer that lacks readonly_buffer usage.
    auto const uniform_only = make_buffer(64, sg::buffer_usage::uniform_buffer);
    CHECK_ASSERTS(uniform_only->as_readonly_buffer<sg::u32>());

    // Out-of-bounds range.
    auto const small = make_buffer(16, sg::buffer_usage::readonly_buffer);
    CHECK_ASSERTS(small->as_readonly_buffer<sg::u32>({.offset = 0, .size = 100}));
}
