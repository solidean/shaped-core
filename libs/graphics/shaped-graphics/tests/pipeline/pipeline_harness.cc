#include "pipeline_harness.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/texture.hh>
#include <typed-geometry/scalar/half_float.hh>

using namespace cc::primitive_defines;

namespace
{
/// A texel of `N` channels of `T`, read as one value.
template <class T, int N = 4>
struct texel
{
    T v[N];
};

cc::shared_async<sg_test::offscreen_pixels> read_back(cc::vector<sg::bytes_future> futures, sg_test::offscreen desc)
{
    auto result = sg_test::offscreen_pixels();
    for (isize i = 0; i < futures.size(); ++i)
    {
        auto bytes = co_await futures[i].bytes();
        result.colors.push_back(sg_test::target_pixels{
            .format = desc.colors[i],
            .width = desc.width,
            .height = desc.height,
            .bytes = cc::vector<byte>::create_copy_of(bytes),
        });
    }
    co_return result;
}
} // namespace

namespace sg_test
{
sg::rendering_info offscreen_targets::cleared() const
{
    auto info = sg::rendering_info{.target_set = description->target_set};
    for (auto const& c : colors)
        info.color_targets.push_back(
            sg::texture_2d::from_raw(c).as_render_target_view().cleared(description->clear_color));
    if (depth_stencil != nullptr)
        info.depth_stencil_target = sg::texture_2d::from_raw(depth_stencil)
                                        .as_depth_stencil_view()
                                        .cleared(description->clear_depth, description->clear_stencil);
    return info;
}

sg::rendering_info offscreen_targets::preserved() const
{
    auto info = sg::rendering_info{.target_set = description->target_set};
    for (auto const& c : colors)
        info.color_targets.push_back(sg::texture_2d::from_raw(c).as_render_target_view().preserved());
    if (depth_stencil != nullptr)
        info.depth_stencil_target = sg::texture_2d::from_raw(depth_stencil).as_depth_stencil_view().preserved();
    return info;
}

cc::shared_async<offscreen_pixels> draw_offscreen_passes(
    sg::context& ctx,
    offscreen desc,
    cc::function_ref<void(sg::command_list&, offscreen_targets const&)> record)
{
    CC_ASSERT(desc.width > 0 && desc.height > 0, "an offscreen target has a size");

    auto targets = offscreen_targets{.description = &desc};
    for (auto const format : desc.colors)
        targets.colors.push_back(
            ctx.persistent
                .create_texture_2d({.format = format,
                                    .width = desc.width,
                                    .height = desc.height,
                                    .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src})
                .raw());
    if (desc.depth_stencil.has_value())
        targets.depth_stencil = ctx.persistent
                                    .create_texture_2d({.format = desc.depth_stencil.value(),
                                                        .width = desc.width,
                                                        .height = desc.height,
                                                        .usage = sg::texture_usage::depth_stencil})
                                    .raw();

    auto cmd = ctx.create_command_list();
    record(*cmd, targets);
    auto futures = cc::vector<sg::bytes_future>();
    for (auto const& c : targets.colors)
        futures.push_back(cmd->download.bytes_from_texture(c));
    ctx.submit_command_list(cc::move(cmd));

    return read_back(cc::move(futures), cc::move(desc));
}

cc::shared_async<offscreen_pixels> draw_offscreen(sg::context& ctx,
                                                  offscreen desc,
                                                  cc::function_ref<void(sg::rendering_scope&)> record)
{
    return draw_offscreen_passes(ctx, cc::move(desc),
                                 [&](sg::command_list& cmd, offscreen_targets const& targets)
                                 {
                                     auto scope = cmd.raster.render_to(targets.cleared());
                                     record(scope);
                                 });
}

tg::vec4i target_pixels::rgba8(int x, int y) const
{
    auto const t = at<texel<u8>>(x, y);
    return tg::vec4i(t.v[0], t.v[1], t.v[2], t.v[3]);
}

tg::vec4f target_pixels::rgba_float(int x, int y) const
{
    switch (format)
    {
    case sg::pixel_format::r32_float:
        return tg::vec4f(at<float>(x, y), 0, 0, 0);
    case sg::pixel_format::rg32_float:
    {
        auto const t = at<texel<float, 2>>(x, y);
        return tg::vec4f(t.v[0], t.v[1], 0, 0);
    }
    case sg::pixel_format::rgba32_float:
    {
        auto const t = at<texel<float>>(x, y);
        return tg::vec4f(t.v[0], t.v[1], t.v[2], t.v[3]);
    }
    case sg::pixel_format::rgba16_float:
    {
        auto const t = at<texel<u16>>(x, y);
        auto const f = [](u16 bits) { return tg::f16::make_from_bits(bits).to_f32(); };
        return tg::vec4f(f(t.v[0]), f(t.v[1]), f(t.v[2]), f(t.v[3]));
    }
    default:
        CC_UNREACHABLE("rgba_float reads float targets only");
    }
}

tg::vec4f clip_rect(int x0, int y0, int x1, int y1, int width, int height)
{
    auto const x = [&](int px) { return -1.0f + 2.0f * float(px) / float(width); };
    auto const y = [&](int py) { return 1.0f - 2.0f * float(py) / float(height); };
    return tg::vec4f(x(x0), y(y1), x(x1), y(y0));
}
} // namespace sg_test
