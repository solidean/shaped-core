#include <clean-core/fwd.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/index_buffer_view.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/types.hh>

using namespace cc::primitive_defines;

// `sg::index_buffer_offset_alignment`, pinned as an sg rule rather than as the one backend's that minds.
//
// **Metal is the backend that minds, and it does not report it.** It names the indices by GPU address, so sg's first
// index is folded into that address — and a misaligned one comes back as part of the mesh drawn, with no error and no
// validation message.
// D3D12 and Vulkan take an odd first index without complaint, so a rule enforced only where it is needed is a rule
// nobody developing on Windows ever meets, in code that then draws wrong on a Mac.
//
// The rule is sg's and **every backend asserts it**, which is the convention
// libs/graphics/shaped-graphics/docs/writing-a-backend.md states for contracts sg does not validate before the seam.
// The truth table below is device-free, so it holds on a host with no GPU at all.
// The two refusals under it are invocable, which is what holds each backend to carrying the check — a backend that
// forgot fails them rather than quietly accepting a draw that another backend rejects.
// See libs/graphics/shaped-graphics/docs/concepts/raster-pipeline.md.

namespace
{
/// A 4x4 render target to open a rendering scope over — the scope is the precondition a bind and a draw both carry.
/// Nothing here submits, so its contents are never read.
[[nodiscard]] auto make_target(sg::context_handle const& ctx)
{
    return ctx->persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = 4,
        .height = 4,
        .usage = sg::texture_usage::render_target,
    });
}

[[nodiscard]] auto make_indices(sg::context_handle const& ctx, isize count)
{
    return ctx->persistent.create_raw_buffer(count * isize(sizeof(u16)),
                                             sg::buffer_usage::index_buffer | sg::buffer_usage::copy_dst);
}
} // namespace

TEST("sg - index_size_in_bytes is the element width the alignment rule counts in")
{
    CHECK(sg::index_size_in_bytes(sg::index_format::uint16) == 2);
    CHECK(sg::index_size_in_bytes(sg::index_format::uint32) == 4);
}

TEST("sg - is_aligned_index_fetch takes the view offset and the first index together")
{
    constexpr auto u16_format = sg::index_format::uint16;
    constexpr auto u32_format = sg::index_format::uint32;

    // 16-bit indices: every other first index breaks the rule, which is the whole of it.
    CHECK(sg::is_aligned_index_fetch(u16_format, 0, 0));
    CHECK(!sg::is_aligned_index_fetch(u16_format, 0, 1));
    CHECK(sg::is_aligned_index_fetch(u16_format, 0, 2));
    CHECK(!sg::is_aligned_index_fetch(u16_format, 0, 3));

    // 32-bit indices cannot break it: every index of one is already 4 bytes wide.
    CHECK(sg::is_aligned_index_fetch(u32_format, 0, 1));
    CHECK(sg::is_aligned_index_fetch(u32_format, 0, 7));

    // **The two halves are one sum, not two rules.** An aligned view plus an odd first index is misaligned, and a
    // misaligned view plus an odd first index is aligned again — which is why neither can be checked on its own.
    CHECK(sg::is_aligned_index_fetch(u16_format, 4, 2));
    CHECK(!sg::is_aligned_index_fetch(u16_format, 4, 1));
    CHECK(sg::is_aligned_index_fetch(u16_format, 2, 1));
}

INVOCABLE_TEST("sg - an odd first index into a 16-bit index buffer is refused", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const target = make_target(ctx);
    auto const indices = make_indices(ctx, 8);

    auto cmd = ctx->create_command_list();
    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);

        scope.bind_index_buffer({.buffer = indices, .format = sg::index_format::uint16});

        // An aligned view, and still a misaligned fetch: `index_range.offset` counts indices, so index 1 of a 16-bit
        // buffer starts 2 bytes in.
        // That is why the bind cannot carry the rule by itself.
        CHECK_ASSERTS(scope.draw_indexed({.index_range = {.offset = 1, .size = 3}}));
    }
    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg - a misaligned index_buffer_view is refused at the bind", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const target = make_target(ctx);
    auto const indices = make_indices(ctx, 8);

    auto cmd = ctx->create_command_list();
    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);

        // The view's offset is the other half of the same sum, caught where it is given rather than at the draw that
        // would inherit it.
        CHECK_ASSERTS(
            scope.bind_index_buffer({.buffer = indices, .format = sg::index_format::uint16, .offset_in_bytes = 2}));
    }
    ctx->drop_command_list(cc::move(cmd));
}
