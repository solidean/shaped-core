#include <nexus/test.hh>
#include <shaped-graphics/buffer.hh> // sg::buffer<T> — exact/clamped wrapping + reinterpret_as
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/views.hh>

#include <memory>
#include <variant> // std::get — the erased raw_view is a variant; buffer views live in its raw_buffer_view arm

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
    auto const b = sg::buffer<particle>::from_raw(buf);

    auto const v = b.as_uniform_buffer();
    CHECK(v.buffer == buf);
    CHECK(v.offset_in_bytes == 0);
    CHECK(v.size_in_bytes == sizeof(particle));
    CHECK(sg::uniform_buffer_view<particle>::access == sg::view_class::uniform);

    auto const raw = std::get<sg::raw_buffer_view>(v.to_raw());
    CHECK(raw.access == sg::view_class::uniform);
    CHECK(raw.shape == sg::view_shape::uniform_block);
    CHECK(raw.buffer == buf);
    CHECK(raw.size_in_bytes == sizeof(particle));

    // element_index selects one block of a UBO array; byte offset = index * sizeof(particle) (must be 256-aligned).
    auto const at = b.as_uniform_buffer(16); // element 16 -> byte 256
    CHECK(at.offset_in_bytes == 256);
    CHECK(at.size_in_bytes == sizeof(particle));

    // element 1 -> byte 16, not 256-aligned -> asserts.
    CHECK_ASSERTS(b.as_uniform_buffer(1));

    // element 64 -> byte 1024, aligned but past the end -> asserts.
    CHECK_ASSERTS(b.as_uniform_buffer(64));
}

TEST("sg views - readonly structured view")
{
    auto const buf = make_buffer(sg::isize(sizeof(particle)) * 10, sg::buffer_usage::readonly_buffer);

    // Whole buffer: element_count == size / sizeof(T).
    auto const whole = sg::buffer<particle>::from_raw(buf).as_readonly_buffer();
    CHECK(whole.offset_in_bytes == 0);
    CHECK(whole.element_count == 10);

    auto const raw = std::get<sg::raw_buffer_view>(whole.to_raw());
    CHECK(raw.access == sg::view_class::readonly);
    CHECK(raw.shape == sg::view_shape::structured);
    CHECK(raw.element_count == 10);
    CHECK(raw.stride_in_bytes == sizeof(particle));

    // Sub-range in elements: offset is scaled by the stride.
    auto const sub = sg::buffer<sg::u32>::from_raw(buf).as_readonly_buffer({.offset = 2, .size = 3});
    CHECK(sub.offset_in_bytes == 2 * sizeof(sg::u32));
    CHECK(sub.element_count == 3);
    CHECK(std::get<sg::raw_buffer_view>(sub.to_raw()).stride_in_bytes == sizeof(sg::u32));
}

TEST("sg views - readwrite structured view")
{
    auto const buf = make_buffer(sg::isize(sizeof(particle)) * 4, sg::buffer_usage::readwrite_buffer);

    auto const v = sg::buffer<particle>::from_raw(buf).as_readwrite_buffer();
    CHECK(v.element_count == 4);

    auto const raw = std::get<sg::raw_buffer_view>(v.to_raw());
    CHECK(raw.access == sg::view_class::readwrite);
    CHECK(raw.shape == sg::view_shape::structured);
    CHECK(raw.stride_in_bytes == sizeof(particle));
}

TEST("sg views - raw byte views")
{
    auto const buf = make_buffer(256, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer);

    // The raw shader views return the erased raw_buffer_view directly (raw_buffer is the low-level path).
    auto const ro = buf->as_raw_readonly();
    CHECK(ro.access == sg::view_class::readonly);
    CHECK(ro.shape == sg::view_shape::raw);
    CHECK(ro.size_in_bytes == 256);
    CHECK(ro.stride_in_bytes == 0);

    auto const rw = buf->as_raw_readwrite();
    CHECK(rw.access == sg::view_class::readwrite);
    CHECK(rw.shape == sg::view_shape::raw);
    CHECK(rw.size_in_bytes == 256);

    // Subrange, in bytes.
    auto const sub = buf->as_raw_readonly({.offset = 64, .size = 128});
    CHECK(sub.offset_in_bytes == 64);
    CHECK(sub.shape == sg::view_shape::raw);
    CHECK(sub.size_in_bytes == 128);

    // Strided variant -> a structured view with an explicit byte stride (no C++ element type).
    auto const structured = buf->as_raw_readonly({.offset = 0, .size = 128}, 16);
    CHECK(structured.shape == sg::view_shape::structured);
    CHECK(structured.element_count == 8); // 128 / 16
    CHECK(structured.stride_in_bytes == 16);
}

TEST("sg views - implicit conversion to raw_view")
{
    auto const buf = make_buffer(64, sg::buffer_usage::readonly_buffer);

    // The typed view converts implicitly to the erased form a backend consumes.
    sg::raw_view const rv = sg::buffer<sg::u32>::from_raw(buf).as_readonly_buffer();
    CHECK(sg::shape_of(rv) == sg::view_shape::structured);
    CHECK(std::get<sg::raw_buffer_view>(rv).element_count == 16);
}

TEST("sg views - empty buffer yields empty view")
{
    // Size 0 is a valid empty buffer; a whole-buffer view over it is a zero-element view.
    auto const buf = make_buffer(0, sg::buffer_usage::readonly_buffer);

    auto const v = sg::buffer<particle>::from_raw(buf).as_readonly_buffer();
    CHECK(v.element_count == 0);
}

TEST("sg views - misuse asserts")
{
    // Wrong usage: a readonly view over a buffer that lacks readonly_buffer usage.
    auto const uniform_only = make_buffer(64, sg::buffer_usage::uniform_buffer);
    CHECK_ASSERTS(sg::buffer<sg::u32>::from_raw(uniform_only).as_readonly_buffer());

    // Out-of-bounds range.
    auto const small = make_buffer(16, sg::buffer_usage::readonly_buffer);
    CHECK_ASSERTS(sg::buffer<sg::u32>::from_raw(small).as_readonly_buffer({.offset = 0, .size = 100}));
}

TEST("sg views - access-erased buffer_view<T> middle")
{
    auto const buf
        = make_buffer(sizeof(particle) * 4, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer);

    // The fully-typed leaves convert implicitly to the access-erased middle (access is a runtime field).
    sg::buffer_view<particle> const ro = sg::buffer<particle>::from_raw(buf).as_readonly_buffer();
    CHECK(ro.access == sg::view_class::readonly);
    CHECK(ro.shape == sg::view_shape::structured);
    CHECK(ro.element_count == 4);
    CHECK(ro.stride_in_bytes == sizeof(particle));
    CHECK(sg::access_of(ro.to_raw()) == sg::view_class::readonly);

    sg::buffer_view<particle> const rw = sg::buffer<particle>::from_raw(buf).as_readwrite_buffer();
    CHECK(rw.access == sg::view_class::readwrite);

    // Uniform too — particle is a uniform_element.
    auto const ubuf = make_buffer(256, sg::buffer_usage::uniform_buffer);
    sg::buffer_view<particle> const u = sg::buffer<particle>::from_raw(ubuf).as_uniform_buffer();
    CHECK(u.access == sg::view_class::uniform);
    CHECK(u.shape == sg::view_shape::uniform_block);
    CHECK(u.size_in_bytes == sizeof(particle));
}

TEST("sg - buffer<T> from_raw / from_raw_clamped wrapping and reinterpret_as")
{
    // particle is 16 bytes; 64 bytes is exactly 4 particles.
    auto const raw = make_buffer(64, sg::buffer_usage::readonly_buffer);
    CHECK(sg::buffer<particle>::from_raw(raw).element_count() == 4);

    // from_raw_clamped tolerates a trailing partial element; from_raw asserts / try_from_raw fails on it.
    auto const raw_partial = make_buffer(70, sg::buffer_usage::readonly_buffer); // 4 particles + 6 bytes
    CHECK(sg::buffer<particle>::from_raw_clamped(raw_partial).element_count() == 4);
    CHECK_ASSERTS(sg::buffer<particle>::from_raw(raw_partial));
    CHECK(!sg::buffer<particle>::try_from_raw(raw_partial).has_value());
    CHECK(sg::buffer<particle>::try_from_raw(raw).value().element_count() == 4);

    // reinterpret_as is compile-time-legal only when U tiles T (u32 divides particle): 4 particles -> 16 u32.
    auto const b = sg::buffer<particle>::from_raw(raw);
    CHECK(b.reinterpret_as<sg::u32>().element_count() == 16);
    // The other direction (U larger) goes through try_reinterpret_as (runtime size check).
    CHECK(b.reinterpret_as<sg::u32>().try_reinterpret_as<particle>().value().element_count() == 4);

    // A byte buffer -> any U is the general case: 70 % 16 != 0 (nullopt) but 70 % 2 == 0.
    auto const bytes = sg::buffer<sg::byte>::from_raw(raw_partial);
    CHECK(!bytes.try_reinterpret_as<particle>().has_value());
    CHECK(bytes.try_reinterpret_as<sg::u16>().value().element_count() == 35);
}

TEST("sg views - raw byte-level as_* variants")
{
    // Raw uniform: an explicit byte range -> the erased raw_buffer_view (uniform_block).
    auto const ubuf = make_buffer(1024, sg::buffer_usage::uniform_buffer);
    auto const u = ubuf->as_raw_uniform_buffer({.offset = 256, .size = 64});
    CHECK(u.access == sg::view_class::uniform);
    CHECK(u.shape == sg::view_shape::uniform_block);
    CHECK(u.offset_in_bytes == 256);
    CHECK(u.size_in_bytes == 64);
    CHECK_ASSERTS(ubuf->as_raw_uniform_buffer({.offset = 16, .size = 64})); // offset not 256-aligned

    // Raw vertex: explicit byte range + explicit stride.
    auto const vbuf = make_buffer(256, sg::buffer_usage::vertex_buffer);
    auto const v = vbuf->as_raw_vertex_buffer({.offset = 32, .size = 96}, 24);
    CHECK(v.offset_in_bytes == 32);
    CHECK(v.size_in_bytes == 96);
    CHECK(v.stride_in_bytes == 24);

    // Raw index: format + explicit byte range.
    auto const ibuf = make_buffer(256, sg::buffer_usage::index_buffer);
    auto const i = ibuf->as_raw_index_buffer(sg::index_format::uint32, {.offset = 8, .size = 40});
    CHECK(i.format == sg::index_format::uint32);
    CHECK(i.offset_in_bytes == 8);
    CHECK(i.size_in_bytes == 40);
}
