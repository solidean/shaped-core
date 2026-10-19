#include <babel-serializer/image/bcn.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/container/fixed_array.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-viewer/resources/impl/mip_layout.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/resources/value_summary.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sv
{
namespace
{
/// How one texel of a decodable format is stored.
struct texel_layout
{
    i32 channels = 0;
    i32 bytes_per_channel = 0;
    bool srgb = false;
    /// blue first, as `bgra8_*` stores it
    bool swapped = false;
};

[[nodiscard]] cc::optional<texel_layout> layout_of(sg::pixel_format format)
{
    using pf = sg::pixel_format;
    switch (format)
    {
    case pf::r8_unorm:
        return texel_layout{.channels = 1, .bytes_per_channel = 1};
    case pf::rg8_unorm:
        return texel_layout{.channels = 2, .bytes_per_channel = 1};
    case pf::rgba8_unorm:
        return texel_layout{.channels = 4, .bytes_per_channel = 1};
    case pf::rgba8_unorm_srgb:
        return texel_layout{.channels = 4, .bytes_per_channel = 1, .srgb = true};
    case pf::bgra8_unorm:
        return texel_layout{.channels = 4, .bytes_per_channel = 1, .swapped = true};
    case pf::bgra8_unorm_srgb:
        return texel_layout{.channels = 4, .bytes_per_channel = 1, .srgb = true, .swapped = true};
    case pf::r16_float:
        return texel_layout{.channels = 1, .bytes_per_channel = 2};
    case pf::rg16_float:
        return texel_layout{.channels = 2, .bytes_per_channel = 2};
    case pf::rgba16_float:
        return texel_layout{.channels = 4, .bytes_per_channel = 2};
    case pf::r32_float:
        return texel_layout{.channels = 1, .bytes_per_channel = 4};
    case pf::rg32_float:
        return texel_layout{.channels = 2, .bytes_per_channel = 4};
    case pf::rgba32_float:
        return texel_layout{.channels = 4, .bytes_per_channel = 4};
    default:
        return {};
    }
}

/// What an 8-bit value reads as, once as a unorm and once as an sRGB color decoded to linear.
struct byte_tables
{
    cc::fixed_array<f32, 256> unorm = {};
    cc::fixed_array<f32, 256> srgb = {};
};

[[nodiscard]] byte_tables const& tables()
{
    static auto const t = []
    {
        auto out = byte_tables();
        for (auto i = 0; i < 256; ++i)
        {
            auto const c = f32(i) / 255.0f;
            out.unorm[i] = c;
            out.srgb[i] = c <= 0.04045f ? c / 12.92f : tg::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return out;
    }();
    return t;
}

/// The texel at `p` as the sampler returns it.
[[nodiscard]] tg::vec4f decode_texel(byte const* p, texel_layout const& l, byte_tables const& t)
{
    auto out = tg::vec4f(0, 0, 0, 1);
    for (auto c = 0; c < l.channels; ++c)
    {
        auto const* const at = p + c * l.bytes_per_channel;
        auto value = 0.0f;
        if (l.bytes_per_channel == 1)
        {
            // Alpha is linear in an sRGB format too.
            value = (l.srgb && c < 3 ? t.srgb : t.unorm)[u8(*at)];
        }
        else if (l.bytes_per_channel == 2)
        {
            auto bits = u16(0);
            cc::memcpy(&bits, at, 2);
            value = tg::half_float::make_from_bits(bits).to_f32();
        }
        else
        {
            cc::memcpy(&value, at, 4);
        }
        out[c] = value;
    }
    if (l.swapped)
    {
        auto const blue = out[0];
        out[0] = out[2];
        out[2] = blue;
    }
    return out;
}

/// Sums and ranges over the values one scan reads, a non-finite value skipped, since it would poison the mean.
struct accumulator
{
    cc::fixed_array<f64, 4> sum = {0.0, 0.0, 0.0, 0.0};
    tg::vec4f min = tg::vec4f(0, 0, 0, 0);
    tg::vec4f max = tg::vec4f(0, 0, 0, 0);
    isize count = 0;
    bool skipped = false;

    void add(tg::vec4f v)
    {
        for (auto c = 0; c < 4; ++c)
        {
            if (!tg::is_finite(v[c]))
            {
                skipped = true;
                return;
            }
        }
        for (auto c = 0; c < 4; ++c)
        {
            sum[c] += f64(v[c]);
            min[c] = count == 0 ? v[c] : cc::min(min[c], v[c]);
            max[c] = count == 0 ? v[c] : cc::max(max[c], v[c]);
        }
        ++count;
    }

    [[nodiscard]] cc::optional<value_summary> finish(bool every_value_read) const
    {
        if (count == 0)
            return {};
        auto mean = tg::vec4f(0, 0, 0, 0);
        for (auto c = 0; c < 4; ++c)
            mean[c] = f32(sum[c] / f64(count));
        return value_summary{.mean = mean, .min = min, .max = max, .exact = every_value_read && !skipped};
    }
};

/// The cells of a `width` x `height` grid a scan within `budget` cells reads, row by row.
/// Every cell where the grid fits the budget, and otherwise every `stride`-th row, read in runs of `run` adjacent cells spaced `stride` runs apart.
/// The runs keep the reads of one row within a few cache lines, where single cells `stride` apart would each touch their own.
/// Each row read starts its runs one run further along than the row before, so a narrow tall level has every column read in time.
/// A budget below one run reads one run, of the first row alone at worst.
struct scan_grid
{
    i32 stride = 1;
    i32 run = 1;

    [[nodiscard]] static scan_grid of(i32 width, i32 height, isize budget, i32 max_run)
    {
        auto const run = cc::min(width, max_run);
        // One run is what the coarsest grid still reads, so a smaller budget could never be met.
        auto const limit = cc::max(budget, isize(run));
        auto const cost = [&](i32 s)
        {
            auto const rows = (isize(height) + s - 1) / s;
            auto const step = isize(s) * isize(run);
            auto const runs = (isize(width) + step - 1) / step;
            return s == 1 ? isize(width) * isize(height) : rows * runs * isize(run);
        };
        auto stride = 1;
        while (cost(stride) > limit)
            ++stride;
        return {.stride = stride, .run = stride == 1 ? width : run};
    }

    [[nodiscard]] bool is_whole() const { return stride == 1; }

    /// Calls `f(x, y)` for every cell the scan reads.
    template <class F>
    void for_each(i32 width, i32 height, F&& f) const
    {
        auto const step = stride == 1 ? width : stride * run;
        // The row's first run moves along by one run per row read, within one step and within the row.
        auto const period = cc::min(step, width);
        for (auto y = 0; y < height; y += stride)
        {
            auto const first = i32((isize(y / stride) * isize(run)) % isize(period));
            for (auto x0 = first; x0 < width; x0 += step)
                for (auto x = x0; x < cc::min(x0 + run, width); ++x)
                    f(x, y);
        }
    }
};

/// Decodes the 16 texels of one block at `p` into `out`, as the sampler returns them; false for a format it does not decode.
[[nodiscard]] bool decode_block(byte const* p,
                                sg::pixel_format format,
                                byte_tables const& t,
                                cc::fixed_array<tg::vec4f, 16>& out)
{
    using pf = sg::pixel_format;
    auto const from_bytes = [&](babel::bcn::rgba8_block const& b, bool srgb)
    {
        for (auto i = 0; i < 16; ++i)
        {
            auto const& lut = srgb ? t.srgb : t.unorm;
            out[i] = tg::vec4f(lut[b.rgba[i * 4 + 0]], lut[b.rgba[i * 4 + 1]], lut[b.rgba[i * 4 + 2]],
                               t.unorm[b.rgba[i * 4 + 3]]);
        }
    };
    auto const from_floats = [&](babel::bcn::rgba32f_block const& b)
    {
        for (auto i = 0; i < 16; ++i)
            out[i] = tg::vec4f(b.rgba[i * 4 + 0], b.rgba[i * 4 + 1], b.rgba[i * 4 + 2], b.rgba[i * 4 + 3]);
    };

    switch (format)
    {
    case pf::bc1_rgba_unorm:
    case pf::bc1_rgba_unorm_srgb:
        from_bytes(babel::bcn::decode_bc1(babel::bcn::small_block(p)), format == pf::bc1_rgba_unorm_srgb);
        return true;
    case pf::bc2_unorm:
    case pf::bc2_unorm_srgb:
        from_bytes(babel::bcn::decode_bc2(babel::bcn::large_block(p)), format == pf::bc2_unorm_srgb);
        return true;
    case pf::bc3_unorm:
    case pf::bc3_unorm_srgb:
        from_bytes(babel::bcn::decode_bc3(babel::bcn::large_block(p)), format == pf::bc3_unorm_srgb);
        return true;
    case pf::bc7_rgba_unorm:
    case pf::bc7_rgba_unorm_srgb:
        from_bytes(babel::bcn::decode_bc7(babel::bcn::large_block(p)), format == pf::bc7_rgba_unorm_srgb);
        return true;
    case pf::bc4_r_unorm:
    case pf::bc4_r_snorm:
        from_floats(babel::bcn::decode_bc4_f32(babel::bcn::small_block(p), format == pf::bc4_r_snorm));
        return true;
    case pf::bc5_rg_unorm:
    case pf::bc5_rg_snorm:
        from_floats(babel::bcn::decode_bc5_f32(babel::bcn::large_block(p), format == pf::bc5_rg_snorm));
        return true;
    case pf::bc6h_rgb_ufloat:
    case pf::bc6h_rgb_sfloat:
        from_floats(babel::bcn::decode_bc6h(babel::bcn::large_block(p), format == pf::bc6h_rgb_sfloat));
        return true;
    default:
        return false;
    }
}

/// `summarize_texels` for a block-compressed level: its blocks decoded as the sampler reads them, on a grid of blocks within the budget.
/// Never exact: babel's decoders match hardware to the tolerance the specification allows, which is no proof of darkness.
[[nodiscard]] cc::optional<value_summary> summarize_blocks(cc::span<byte const> level_bytes,
                                                           sg::pixel_format format,
                                                           i32 width,
                                                           i32 height,
                                                           isize max_texels)
{
    auto const block_bytes = isize(sg::format_block_size(format));
    auto const blocks_x = (width + 3) / 4;
    auto const blocks_y = (height + 3) / 4;
    if (level_bytes.size() < isize(blocks_x) * isize(blocks_y) * block_bytes)
        return {};

    auto const& t = tables();
    auto acc = accumulator();
    auto decodable = true;
    // Four blocks are one 64-byte run of BC2 - BC7, two of BC1 and BC4.
    auto const grid = scan_grid::of(blocks_x, blocks_y, max_texels / 16, 4);
    grid.for_each(blocks_x, blocks_y,
                  [&](i32 bx, i32 by)
                  {
                      auto const* const p = level_bytes.data() + (isize(by) * isize(blocks_x) + isize(bx)) * block_bytes;
                      auto decoded = cc::fixed_array<tg::vec4f, 16>{};
                      if (!decode_block(p, format, t, decoded))
                      {
                          decodable = false;
                          return;
                      }
                      // A block at the level's edge decodes texels past it, which nothing samples.
                      for (auto i = 0; i < 16; ++i)
                          if (bx * 4 + i % 4 < width && by * 4 + i / 4 < height)
                              acc.add(decoded[i]);
                  });
    if (!decodable)
        return {};
    return acc.finish(false);
}
} // namespace

cc::optional<value_summary> summarize_texels(cc::span<byte const> level_bytes,
                                             sg::pixel_format format,
                                             i32 width,
                                             i32 height,
                                             isize max_texels)
{
    if (width <= 0 || height <= 0)
        return {};
    if (sg::is_compressed_format(format))
        return summarize_blocks(level_bytes, format, width, height, max_texels);

    auto const layout = layout_of(format);
    if (!layout.has_value())
        return {};
    auto const texel_bytes = isize(layout.value().channels * layout.value().bytes_per_channel);
    if (level_bytes.size() < isize(width) * isize(height) * texel_bytes)
        return {};

    auto const& t = tables();
    auto acc = accumulator();
    // Sixteen texels are one 64-byte line of rgba8.
    auto const grid = scan_grid::of(width, height, max_texels, 16);
    grid.for_each(width, height,
                  [&](i32 x, i32 y)
                  {
                      acc.add(decode_texel(level_bytes.data() + (isize(y) * isize(width) + isize(x)) * texel_bytes,
                                           layout.value(), t));
                  });
    return acc.finish(grid.is_whole());
}

cc::optional<value_summary> summarize_texture(texture_data const& texture)
{
    auto const last_level = texture.mip_count - 1;
    auto last_offset = isize(0);
    for (auto mip = i32(0); mip < last_level; ++mip)
        last_offset += impl::mip_byte_size(texture.format, texture.width, texture.height, mip);
    auto const last_size = impl::mip_byte_size(texture.format, texture.width, texture.height, last_level);
    if (last_offset + last_size > texture.pixels.span().size())
        return {};
    auto summary = summarize_texels(
        texture.pixels.span().subspan(cc::offset_size{.offset = last_offset, .size = last_size}), texture.format,
        impl::mip_extent(texture.width, last_level), impl::mip_extent(texture.height, last_level));
    if (summary.has_value() && last_level > 0)
        summary.value().exact = false;
    return summary;
}

cc::optional<value_summary> summarize_elements(cc::span<byte const> bytes,
                                               attribute_format format,
                                               isize count,
                                               isize max_elements)
{
    auto const lanes = format.component_count();
    if (format.scalar != scalar_type::f32 || format.is_matrix() || lanes > 4 || count <= 0)
        return {};
    auto const stride = isize(format.size_bytes());
    if (bytes.size() < count * stride)
        return {};

    auto acc = accumulator();
    // The elements as one row, so a long one is read in runs of 16 on the coarser grid the texel scan uses.
    auto const width = i32(cc::min(count, isize(0x7fffffff)));
    auto const grid = scan_grid::of(width, 1, max_elements, 16);
    grid.for_each(width, 1,
                  [&](i32 i, i32)
                  {
                      auto v = tg::vec4f(0, 0, 0, 0);
                      cc::memcpy(&v, bytes.data() + isize(i) * stride, isize(lanes) * 4);
                      acc.add(v);
                  });
    return acc.finish(grid.is_whole() && isize(width) == count);
}
} // namespace sv
