#include <nexus/test.hh>
#include <shaped-graphics/buffer.hh> // typed buffer<T> wrapper + ctx.*.create_buffer<T>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/raw_buffer.hh> // sg::raw_buffer::size_in_bytes / usage (raw_buffer_handle operator-> target)
#include <shaped-graphics/types.hh>

#include <variant> // std::get — raw_view is a variant; buffer views live in its raw_buffer_view arm

namespace
{
// A representative GPU element: 32 bytes, so a view_element (multiple of 4) and a uniform_element (of 16).
struct particle
{
    float position[4];
    float velocity[4];
};
static_assert(sizeof(particle) == 32);
} // namespace

// Backend-agnostic tests for persistent buffer allocation through `ctx->persistent.create_raw_buffer`. Each is an
// INVOCABLE_TEST run against every available backend (see tests/context/context-test.cc for the mechanism).

INVOCABLE_TEST("sg - allocates a persistent buffer", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);
    CHECK(buf->size_in_bytes() == 256);
    CHECK(sg::has_flag(buf->usage(), sg::buffer_usage::copy_src));
    CHECK(sg::has_flag(buf->usage(), sg::buffer_usage::copy_dst));
    CHECK(!sg::has_flag(buf->usage(), sg::buffer_usage::vertex_buffer));
}

INVOCABLE_TEST("sg - allocates buffers across usages", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // A spread of usages a backend must accept at creation; the shape round-trips through the handle.
    sg::buffer_usage const usages[] = {
        sg::buffer_usage::vertex_buffer,
        sg::buffer_usage::index_buffer,
        sg::buffer_usage::uniform_buffer,
        sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer,
        sg::buffer_usage::copy_src | sg::buffer_usage::readonly_buffer,
    };
    for (auto const u : usages)
    {
        auto buf = ctx->persistent.create_raw_buffer(1024, u);
        REQUIRE(buf != nullptr);
        CHECK(buf->size_in_bytes() == 1024);
        CHECK(buf->usage() == u);
    }
}

INVOCABLE_TEST("sg - zero-size buffer allocates nothing", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // A zero-size buffer is valid and allocates nothing.
    auto empty = ctx->persistent.create_raw_buffer(0, sg::buffer_usage::none);
    REQUIRE(empty != nullptr);
    CHECK(empty->size_in_bytes() == 0);
}

INVOCABLE_TEST("sg - allocates a large buffer", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // A few MiB — comfortably past the inline rings, exercising a real dedicated allocation.
    auto const size = cc::isize(8) * 1024 * 1024;
    auto buf = ctx->persistent.create_raw_buffer(size, sg::buffer_usage::readwrite_buffer);
    REQUIRE(buf != nullptr);
    CHECK(buf->size_in_bytes() == size);
}

// Typed buffer<T>: element_count -> byte size = count * sizeof(T); the wrapper's view factories infer T.

INVOCABLE_TEST("sg - typed create_buffer<T> (persistent)", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::buffer<particle> buf = ctx->persistent.create_buffer<particle>(
        1000, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer);

    REQUIRE(buf.raw() != nullptr);
    CHECK(buf.element_count() == 1000);
    CHECK(buf.size_in_bytes() == 1000 * sg::isize(sizeof(particle)));

    // Views infer the element type from T (no <particle> spelled) and carry the right stride / count.
    auto ro = buf.as_readonly_buffer();
    CHECK(ro.element_count == 1000);
    auto raw_ro = std::get<sg::raw_buffer_view>(ro.to_raw());
    CHECK(raw_ro.stride_in_bytes == sg::isize(sizeof(particle)));
    CHECK(raw_ro.element_count == 1000);

    // A storage view's byte offset must be 256-byte aligned, so the element offset must be a multiple of
    // 256 / sizeof(particle) = 8 (element 96 -> byte 3072).
    auto rw = buf.as_readwrite_buffer({.offset = 96, .size = 50});
    CHECK(rw.element_count == 50);
    CHECK(rw.offset_in_bytes == 96 * sg::isize(sizeof(particle)));
    CHECK_ASSERTS(buf.as_readwrite_buffer({.offset = 100, .size = 50})); // byte 3200 — not 256-aligned
}

INVOCABLE_TEST("sg - typed create_buffer<T> (transient)", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::buffer<particle> buf = ctx->transient.create_buffer<particle>(64, sg::buffer_usage::readwrite_buffer);

    REQUIRE(buf.raw() != nullptr);
    CHECK(buf.raw()->is_valid());
    CHECK(buf.element_count() == 64);
}

INVOCABLE_TEST("sg - typed try_create_buffer<T> (persistent)", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto r = ctx->persistent.try_create_buffer<particle>(8, sg::buffer_usage::readonly_buffer);
    REQUIRE(r.has_value());
    CHECK(r.value().element_count() == 8);
}

INVOCABLE_TEST("sg - typed buffer<u16> as_index_buffer picks the index width", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::buffer<sg::u16> indices = ctx->persistent.create_buffer<sg::u16>(300, sg::buffer_usage::index_buffer);

    CHECK(indices.element_count() == 300);
    auto ib = indices.as_index_buffer();
    CHECK(ib.format == sg::index_format::uint16);
    CHECK(ib.offset_in_bytes == 0);

    // Subrange: in indices; byte offset / size follow the u16 width.
    auto ib_sub = indices.as_index_buffer({.offset = 30, .size = 60});
    CHECK(ib_sub.format == sg::index_format::uint16);
    CHECK(ib_sub.offset_in_bytes == 30 * 2);
    CHECK(ib_sub.size_in_bytes == 60 * 2);
}

INVOCABLE_TEST("sg - typed buffer<T> vertex-buffer subrange is in vertices of T", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::buffer<particle> verts = ctx->persistent.create_buffer<particle>(100, sg::buffer_usage::vertex_buffer);

    auto whole = verts.as_vertex_buffer();
    CHECK(whole.stride_in_bytes == sg::isize(sizeof(particle)));
    CHECK(whole.offset_in_bytes == 0);

    // Subrange: offset / size are in vertices, scaled by the stride.
    auto sub = verts.as_vertex_buffer({.offset = 10, .size = 20});
    CHECK(sub.stride_in_bytes == sg::isize(sizeof(particle)));
    CHECK(sub.offset_in_bytes == 10 * sg::isize(sizeof(particle)));
    CHECK(sub.size_in_bytes == 20 * sg::isize(sizeof(particle)));
}

INVOCABLE_TEST("sg - typed buffer<T>.as_uniform_buffer binds one element by index", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // 16 elements of 32 bytes; the argument is an element index, and the view is a single block of T.
    sg::buffer<particle> ub = ctx->persistent.create_buffer<particle>(16, sg::buffer_usage::uniform_buffer);

    auto v = ub.as_uniform_buffer(8); // element 8 -> byte offset 8 * sizeof(particle) = 256 (256-aligned)
    CHECK(v.offset_in_bytes == 8 * sg::isize(sizeof(particle)));
    CHECK(v.size_in_bytes == sg::isize(sizeof(particle)));
}
