#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_shape.hh>

namespace sr
{
namespace
{
/// Advances `cursor` to a place `count` texels fit on one row, and returns where they start.
[[nodiscard]] tg::pos2i reserve_run(tg::pos2i& cursor, int count)
{
    if (cursor[0] + count > slug_atlas::width)
        cursor = tg::pos2i(0, cursor[1] + 1);
    auto const at = cursor;
    cursor[0] += count;
    return at;
}

[[nodiscard]] bool same_list(cc::vector<i32> const& a, cc::vector<i32> const& b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

template <class Texel>
void grow_rows(cc::vector<Texel>& texels, int& rows, int needed)
{
    if (needed <= rows)
        return;
    texels.resize_to_defaulted(isize(needed) * slug_atlas::width);
    rows = needed;
}

/// Rows the texture is created with: a power of two holding `rows`, so growing is rare.
[[nodiscard]] int capacity_for(int rows)
{
    auto capacity = 16;
    while (capacity < rows)
        capacity *= 2;
    return capacity;
}

template <class Texel>
void upload(sg::command_list& cmd,
            sg::texture_2d& texture,
            sg::pixel_format format,
            cc::vector<Texel> const& texels,
            int rows,
            int& dirty_row)
{
    if (rows == 0)
        return;

    auto& ctx = cmd.context();
    if (texture.raw() == nullptr || texture.height() < rows)
    {
        // A grown texture starts empty, so it takes every row; the old one is released with its epoch.
        texture = ctx.persistent.create_texture_2d({.format = format,
                                                    .width = slug_atlas::width,
                                                    .height = capacity_for(rows),
                                                    .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
        dirty_row = 0;
    }
    if (dirty_row >= rows)
        return;

    auto const first = isize(dirty_row) * slug_atlas::width;
    auto const count = isize(rows - dirty_row) * slug_atlas::width;
    cmd.upload.bytes_to_texture(texture.raw(),
                                cc::span<Texel const>(texels).subspan({.offset = first, .size = count}).as_bytes(), {},
                                sg::texture_region{.offset = tg::pos3i(0, dirty_row, 0),
                                                   .size = tg::vec3i(slug_atlas::width, rows - dirty_row, 1)});
    dirty_row = rows;
}
} // namespace

cc::result<slug_shape_ref> slug_atlas::add(slug_compiled_shape const& shape)
{
    auto ref = slug_shape_ref{.banding = shape.banding, .em_bounds = shape.em_bounds, .em_scale = shape.em_scale};
    if (shape.is_empty())
        return ref;

    auto const header = shape.horizontal_bands.size() + shape.vertical_bands.size();
    if (shape.band_texel_count() > width)
        return cc::error(cc::format("a shape's band data takes {} texels, more than one {}-texel row",
                                    shape.band_texel_count(), width));

    // Where every curve texel lands: each run on one row, so a curve's second texel is always beside its first.
    auto curve_cursor = _curve_cursor;
    auto texel_at = cc::vector<tg::pos2i>::create_defaulted(shape.curve_texels.size());
    auto run_first = i32(0);
    for (auto const run_end : shape.run_ends)
    {
        auto const at = reserve_run(curve_cursor, run_end - run_first);
        for (auto i = run_first; i < run_end; ++i)
            texel_at[i] = tg::pos2i(at[0] + (i - run_first), at[1]);
        run_first = run_end;
    }

    // The band block — header, then every list — on one row, so no list needs the wrap the shader allows.
    auto band_cursor = _band_cursor;
    auto const block = reserve_run(band_cursor, int(shape.band_texel_count()));

    if (curve_cursor[1] >= max_rows || band_cursor[1] >= max_rows)
        return cc::error("the slug atlas is full");

    grow_rows(_curve_texels, _curve_rows, curve_cursor[1] + 1);
    grow_rows(_band_texels, _band_rows, band_cursor[1] + 1);

    // A row an earlier `prepare` uploaded may gain texels now, so the upload restarts at the first row this shape touches.
    _curve_dirty_row = cc::min(_curve_dirty_row, _curve_cursor[1]);
    _band_dirty_row = cc::min(_band_dirty_row, block[1]);

    for (auto i = isize(0); i < shape.curve_texels.size(); ++i)
        _curve_texels[isize(texel_at[i][1]) * width + texel_at[i][0]] = shape.curve_texels[i];

    auto const row = isize(block[1]) * width;
    auto next = isize(header);
    auto entry = isize(0);
    auto write_bands = [&](cc::vector<cc::vector<i32>> const& bands)
    {
        auto previous = isize(-1);
        for (auto b = isize(0); b < bands.size(); ++b)
        {
            // A band listing exactly what the one before it lists points at the same data.
            auto offset = next;
            if (b > 0 && same_list(bands[b], bands[b - 1]))
                offset = previous;
            else
            {
                for (auto const t : bands[b])
                    _band_texels[row + block[0] + next++] = {u16(texel_at[t][0]), u16(texel_at[t][1])};
            }
            _band_texels[row + block[0] + entry++] = {u16(bands[b].size()), u16(offset)};
            previous = offset;
        }
    };
    write_bands(shape.horizontal_bands);
    write_bands(shape.vertical_bands);

    // Nothing past what the block actually used is taken, since a repeated band wrote no list.
    _band_cursor = tg::pos2i(block[0] + int(next), block[1]);
    _curve_cursor = curve_cursor;

    auto const flags = shape.fill_rule == slug_fill_rule::even_odd ? u32(0x1000) : u32(0);
    ref.glyph_location = u32(block[0]) | (u32(block[1]) << 16);
    ref.band_info = u32(shape.vertical_bands.size() - 1) | ((u32(shape.horizontal_bands.size() - 1) | flags) << 16);
    ref.is_drawable = true;
    return ref;
}

void slug_atlas::prepare(sg::command_list& cmd)
{
    upload(cmd, _curve_texture, sg::pixel_format::rgba16_float, _curve_texels, _curve_rows, _curve_dirty_row);
    upload(cmd, _band_texture, sg::pixel_format::rg16_uint, _band_texels, _band_rows, _band_dirty_row);
}
} // namespace sr
