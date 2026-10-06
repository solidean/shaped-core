#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::max
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/compression/bc_compress_routine.hh>
#include <sr_sgl_shaders.hh>

namespace sr
{
namespace
{
/// The `_unorm` member of `format`'s family, which is what an instance of the routine is keyed by.
sg::pixel_format family_of(sg::pixel_format format)
{
    switch (format)
    {
    case sg::pixel_format::bc1_rgba_unorm_srgb:
        return sg::pixel_format::bc1_rgba_unorm;
    case sg::pixel_format::bc3_unorm_srgb:
        return sg::pixel_format::bc3_unorm;
    case sg::pixel_format::bc7_rgba_unorm_srgb:
        return sg::pixel_format::bc7_rgba_unorm;
    default:
        return format;
    }
}

/// Whether a family's blocks are 16 bytes, which is the binding its entry point writes through.
bool is_wide(sg::pixel_format family)
{
    return sg::format_block_size(family) == 16;
}
} // namespace

bool bc_compress_routine::is_supported_target(sg::pixel_format format)
{
    switch (family_of(format))
    {
    case sg::pixel_format::bc1_rgba_unorm:
    case sg::pixel_format::bc3_unorm:
    case sg::pixel_format::bc4_r_unorm:
    case sg::pixel_format::bc5_rg_unorm:
    case sg::pixel_format::bc7_rgba_unorm:
        return true;
    default:
        return false;
    }
}

sg::pixel_format bc_compress_routine::target_for(sg::pixel_format source, sg::pixel_format family)
{
    CC_ASSERT(is_supported_target(family), "not a format bc_compress_routine writes");
    if (!sg::is_srgb_format(source))
        return family_of(family);
    switch (family_of(family))
    {
    case sg::pixel_format::bc1_rgba_unorm:
        return sg::pixel_format::bc1_rgba_unorm_srgb;
    case sg::pixel_format::bc3_unorm:
        return sg::pixel_format::bc3_unorm_srgb;
    case sg::pixel_format::bc7_rgba_unorm:
        return sg::pixel_format::bc7_rgba_unorm_srgb;
    default:
        return family; // no sRGB variant, and `can_compress` refuses the pair
    }
}

bool bc_compress_routine::can_compress(sg::context const& ctx, sg::pixel_format source, sg::pixel_format target)
{
    return ctx.supports(sg::feature::texture_compression_bc) && is_supported_target(target)
        && !sg::is_compressed_format(source) && sg::is_srgb_format(source) == sg::is_srgb_format(target);
}

cc::shared_async<cc::unit> bc_compress_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    auto const& shaders = sgl_shaders::compression_bc;

    // Each entry point is a type of its own, so what init needs of the chosen one is taken out here.
    auto asset = slib::shader_asset_handle();
    auto layout = sg::pipeline_layout_handle();
    switch (params())
    {
    case sg::pixel_format::bc1_rgba_unorm:
        asset = shaders.bc1_cs.asset;
        layout = shaders.bc1_cs.acquire_layout(ctx);
        break;
    case sg::pixel_format::bc3_unorm:
        asset = shaders.bc3_cs.asset;
        layout = shaders.bc3_cs.acquire_layout(ctx);
        break;
    case sg::pixel_format::bc4_r_unorm:
        asset = shaders.bc4_cs.asset;
        layout = shaders.bc4_cs.acquire_layout(ctx);
        break;
    case sg::pixel_format::bc5_rg_unorm:
        asset = shaders.bc5_cs.asset;
        layout = shaders.bc5_cs.acquire_layout(ctx);
        break;
    default:
        asset = shaders.bc7_cs.asset;
        layout = shaders.bc7_cs.acquire_layout(ctx);
        break;
    }

    auto const shader = asset->acquire(ctx);
    co_await cc::async_settled(shader);

    _group_layout = nullptr;
    _pipeline = {};
    auto const* const compiled = shader->try_value();
    if (compiled == nullptr)
    {
        fail_init(); // not pending: this will not come good until a reload, and a caller should be able to tell
        co_return;
    }

    _group_layout = is_wide(params()) ? ctx.cached.acquire_binding_group_layout<sgl_shaders::bc_io16>()
                                      : ctx.cached.acquire_binding_group_layout<sgl_shaders::bc_io8>();
    _pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = layout});
    co_await cc::async_settled(_pipeline);
    co_return;
}

sg::routine_outcome bc_compress_routine::execute(sg::command_list& cmd,
                                                 sg::texture_2d const& source,
                                                 sg::texture_2d const& target,
                                                 int level)
{
    CC_ASSERT(can_compress(cmd.context(), source.format(), target.format()), "a target this routine cannot write from "
                                                                             "this source");
    CC_ASSERT(level >= 0 && level < source.mip_levels() && level < target.mip_levels(), "the level must exist on both");

    auto const family = family_of(target.format());
    auto const self = try_acquire(cmd, family);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto const* const built = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (built == nullptr || *built == nullptr)
        return sg::routine_outcome::declined;

    auto& ctx = cmd.context();

    auto const width = cc::max(1, source.width() >> level);
    auto const height = cc::max(1, source.height() >> level);
    auto const blocks_wide = (width + 3) / 4;
    auto const blocks_high = (height + 3) / 4;
    auto const pitch = sg::buffer_texture_row_pitch(target.format(), width);
    auto const block_bytes = isize(sg::format_block_size(target.format()));
    auto const row_blocks = pitch / block_bytes;
    auto const source_view = source.as_texture_view({.mips = {.start = level, .count = 1}});
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;

    auto blocks = sg::raw_buffer_handle();
    auto group = sg::binding_group_handle();
    if (is_wide(family))
    {
        auto const buffer = ctx.transient.create_buffer<tg::vec<4, u32>>(row_blocks * blocks_high, usage);
        group = ctx.transient.create_binding_group(
            cmd, self->_group_layout,
            sgl_shaders::bc_io16{.source = source_view, .blocks = buffer.as_readwrite_buffer()});
        blocks = buffer.raw();
    }
    else
    {
        auto const buffer = ctx.transient.create_buffer<tg::vec<2, u32>>(row_blocks * blocks_high, usage);
        group = ctx.transient.create_binding_group(
            cmd, self->_group_layout, sgl_shaders::bc_io8{.source = source_view, .blocks = buffer.as_readwrite_buffer()});
        blocks = buffer.raw();
    }

    cmd.compute.bind_pipeline(**built);
    cmd.compute.bind_group(0, *group);
    cmd.compute.set_inline_constants(sgl_shaders::bc_constants{.blocks_wide = blocks_wide,
                                                               .blocks_high = blocks_high,
                                                               .row_blocks = int(row_blocks),
                                                               .width = width,
                                                               .height = height,
                                                               .srgb = sg::is_srgb_format(source.format()) ? 1 : 0}
                                         .to_block());
    cmd.compute.dispatch_threads(blocks_wide, blocks_high);

    cmd.copy.buffer_to_texture({.src = blocks,
                                .dst = target.raw(),
                                .src_row_pitch_in_bytes = pitch,
                                .subresource = {.mip_level = level},
                                .region = sg::texture_region{.offset = {}, .size = tg::vec3i(width, height, 1)}});
    return sg::routine_outcome::executed;
}
} // namespace sr
