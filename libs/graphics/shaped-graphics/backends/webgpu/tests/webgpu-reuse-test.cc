#include "webgpu-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// What a frame asks for again is handed back rather than made again: a browser frees a bind group or a view only when
// its garbage collector runs, and Firefox runs out of descriptors before then.
// Each test uses extents of its own, since the suite shares one context and a leftover of the same description would be claimed.
// libs/graphics/shaped-graphics/docs/issues/bug-sg-webgpu-per-frame-objects-exhaust-firefox.md is why.

namespace
{
namespace webgpu = sg::backend::webgpu;

[[nodiscard]] WGPUTexture wgpu_of(sg::texture_2d const& t)
{
    return std::dynamic_pointer_cast<webgpu::webgpu_texture const>(t.raw())->raw();
}

[[nodiscard]] WGPUBindGroup wgpu_of(sg::binding_group_handle const& g)
{
    return std::dynamic_pointer_cast<webgpu::webgpu_binding_group const>(g)->raw();
}

[[nodiscard]] sg::binding buffer_binding(cc::string_view name)
{
    return sg::binding{
        .name = cc::string(name),
        .group_index = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::buffer,
        .access = sg::access_mode::read_write,
    };
}

[[nodiscard]] sg::binding texture_binding(cc::string_view name)
{
    return sg::binding{
        .name = cc::string(name),
        .group_index = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::texture,
        .access = sg::access_mode::read,
        .texture_dimension = sg::texture_view_dimension::tex_2d,
    };
}
} // namespace

ASYNC_INVOCABLE_TEST("sg webgpu - a transient texture asked for again next epoch is the same WebGPU texture",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    auto const desc = sg::texture_2d_description{.format = sg::pixel_format::r32_float,
                                                 .width = 37,
                                                 .height = 11,
                                                 .usage = sg::texture_usage::render_target | sg::texture_usage::texture};

    auto const first = ctx.transient.create_texture_2d(desc);
    auto const first_wgpu = wgpu_of(first);
    auto const before = ctx.created();
    ctx.advance_epoch();

    auto const second = ctx.transient.create_texture_2d(desc);
    CHECK(first.raw()->is_expired());
    CHECK(wgpu_of(second) == first_wgpu);
    CHECK(ctx.created().textures == before.textures);

    // A different description is no match, and is made.
    auto const other = ctx.transient.create_texture_2d(
        {.format = sg::pixel_format::r32_float, .width = 38, .height = 11, .usage = desc.usage});
    CHECK(wgpu_of(other) != first_wgpu);
    CHECK(ctx.created().textures == before.textures + 1);
    co_return;
}

ASYNC_INVOCABLE_TEST("sg webgpu - a transient texture nobody asks for again is released after one epoch",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    auto const desc = sg::texture_2d_description{.format = sg::pixel_format::r32_float,
                                                 .width = 41,
                                                 .height = 13,
                                                 .usage = sg::texture_usage::render_target | sg::texture_usage::texture};

    (void)ctx.transient.create_texture_2d(desc);
    ctx.advance_epoch(); // claimable now
    ctx.advance_epoch(); // nobody claimed it, so it went

    auto const before = ctx.created();
    (void)ctx.transient.create_texture_2d(desc);
    CHECK(ctx.created().textures == before.textures + 1);
    co_return;
}

ASYNC_INVOCABLE_TEST("sg webgpu - a transient binding group over the same resources next epoch is the same WebGPU "
                     "group",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    auto const layout = ctx.cached.acquire_binding_group_layout({buffer_binding("Output")});

    // The buffer is transient too: it comes back as the same WebGPU buffer under a new handle, and the group's key,
    // which names WebGPU objects by serial, still matches.
    auto const group_over = [&](auto const& buffer)
    {
        sg::named_view const output = {.name = "Output", .view = buffer.as_readwrite_buffer()};
        return ctx.transient.create_binding_group(layout, cc::span<sg::named_view const>(&output, 1));
    };

    auto const first = wgpu_of(group_over(ctx.transient.create_buffer<u32>(43, sg::buffer_usage::readwrite_buffer)));
    auto const before = ctx.created();
    ctx.advance_epoch();

    auto const buffer = ctx.transient.create_buffer<u32>(43, sg::buffer_usage::readwrite_buffer);
    auto const second = group_over(buffer);
    CHECK(wgpu_of(second) == first);
    CHECK(ctx.created().bind_groups == before.bind_groups);
    CHECK(ctx.created().buffers == before.buffers);

    // Asked for again in the same epoch over the same buffer, it is still one group.
    CHECK(wgpu_of(group_over(buffer)) == first);
    CHECK(ctx.created().bind_groups == before.bind_groups);

    // A second transient buffer of the same size in one epoch is a resource of its own, and so is its group.
    (void)group_over(ctx.transient.create_buffer<u32>(43, sg::buffer_usage::readwrite_buffer));
    CHECK(ctx.created().bind_groups == before.bind_groups + 1);
    co_return;
}

ASYNC_INVOCABLE_TEST("sg webgpu - several transient resources of one description come back in the order they were "
                     "asked for, so their groups are reused",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    auto const layout = ctx.cached.acquire_binding_group_layout({buffer_binding("Output")});
    auto const frame = [&]
    {
        // A frame of several views asks for the same buffers in the same order, each into a group of its own.
        for (auto i = 0; i < 3; ++i)
        {
            auto const buffer = ctx.transient.create_buffer<u32>(53, sg::buffer_usage::readwrite_buffer);
            sg::named_view const output = {.name = "Output", .view = buffer.as_readwrite_buffer()};
            (void)ctx.transient.create_binding_group(layout, cc::span<sg::named_view const>(&output, 1));
        }
        ctx.advance_epoch();
    };

    frame();
    auto const before = ctx.created();
    for (auto i = 0; i < 4; ++i)
        frame();
    CHECK(ctx.created().buffers == before.buffers);
    CHECK(ctx.created().bind_groups == before.bind_groups);
    co_return;
}

ASYNC_INVOCABLE_TEST("sg webgpu - a transient dropped mid-frame still comes back to the request it served",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    auto const layout = ctx.cached.acquire_binding_group_layout({buffer_binding("Output")});
    auto const group_over = [&](sg::buffer<u32> const& buffer)
    {
        sg::named_view const output = {.name = "Output", .view = buffer.as_readwrite_buffer()};
        (void)ctx.transient.create_binding_group(layout, cc::span<sg::named_view const>(&output, 1));
    };
    auto const frame = [&]
    {
        // The middle one's handle goes first, so it is offered back before the others expire.
        auto const first = ctx.transient.create_buffer<u32>(59, sg::buffer_usage::readwrite_buffer);
        group_over(first);
        {
            auto const scratch = ctx.transient.create_buffer<u32>(59, sg::buffer_usage::readwrite_buffer);
            group_over(scratch);
        }
        auto const last = ctx.transient.create_buffer<u32>(59, sg::buffer_usage::readwrite_buffer);
        group_over(last);
        ctx.advance_epoch();
    };

    frame();
    auto const before = ctx.created();
    for (auto i = 0; i < 4; ++i)
        frame();
    CHECK(ctx.created().buffers == before.buffers);
    CHECK(ctx.created().bind_groups == before.bind_groups);
    co_return;
}

ASYNC_INVOCABLE_TEST("sg webgpu - a texture's views are cached, and one over part of it goes once unused",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    auto const layout = ctx.cached.acquire_binding_group_layout({texture_binding("source")});
    auto const texture = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                           .width = 47,
                                                           .height = 47,
                                                           .mip_levels = 3,
                                                           .usage = sg::texture_usage::texture});

    // Persistent groups are never reused, so each of these asks the texture for its view again.
    auto const bind = [&](auto const& view)
    {
        sg::named_view const source = {.name = "source", .view = view};
        return ctx.persistent.create_binding_group(layout, cc::span<sg::named_view const>(&source, 1));
    };
    auto const whole = texture.as_texture_view();
    auto const one_mip = texture.as_texture_view({.mips = {.start = 1, .count = 1}});

    auto const before = ctx.created();
    (void)bind(whole);
    (void)bind(whole);
    (void)bind(one_mip);
    (void)bind(one_mip);
    CHECK(ctx.created().texture_views == before.texture_views + 2);

    // Two epochs without a request: the mip's view went, the whole texture's stayed.
    ctx.advance_epoch();
    ctx.advance_epoch();
    (void)bind(whole);
    CHECK(ctx.created().texture_views == before.texture_views + 2);
    (void)bind(one_mip);
    CHECK(ctx.created().texture_views == before.texture_views + 3);

    // The escape hatch: one view dropped by what it covers, then all of them.
    texture.raw()->release_cached_view(
        sg::texture_view_dimension::tex_2d, sg::pixel_format::undefined,
        sg::subresource_range({.start = 1, .end = 2}, {.start = 0, .end = 1}, {.start = 0, .end = 1}));
    (void)bind(one_mip);
    (void)bind(whole);
    CHECK(ctx.created().texture_views == before.texture_views + 4);
    texture.raw()->release_cached_views();
    (void)bind(whole);
    CHECK(ctx.created().texture_views == before.texture_views + 5);
    co_return;
}
