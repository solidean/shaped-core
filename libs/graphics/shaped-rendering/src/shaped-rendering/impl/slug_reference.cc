#include <clean-core/common/utility.hh>
#include <clean-core/math/bit.hh>
#include <shaped-rendering/impl/slug_reference.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sr::impl
{
namespace
{
struct texel4
{
    f32 x = 0;
    f32 y = 0;
    f32 z = 0;
    f32 w = 0;
};

[[nodiscard]] texel4 curve_at(slug_atlas const& atlas, int x, int y)
{
    auto const i = isize(y) * slug_atlas::width + x;
    if (x < 0 || x >= slug_atlas::width || i < 0 || i >= atlas.curve_texels().size())
        return {};
    auto const& t = atlas.curve_texels()[i];
    auto const f = [](u16 bits) { return f32(tg::half_float::make_from_bits(bits)); };
    return {f(t[0]), f(t[1]), f(t[2]), f(t[3])};
}

struct texel2
{
    u32 x = 0;
    u32 y = 0;
};

[[nodiscard]] texel2 band_at(slug_atlas const& atlas, int x, int y)
{
    auto const i = isize(y) * slug_atlas::width + x;
    if (x < 0 || x >= slug_atlas::width || i < 0 || i >= atlas.band_texels().size())
        return {};
    auto const& t = atlas.band_texels()[i];
    return {t[0], t[1]};
}

[[nodiscard]] f32 saturate(f32 v)
{
    return cc::clamp(v, 0.0f, 1.0f);
}

[[nodiscard]] u32 root_code(f32 y1, f32 y2, f32 y3)
{
    auto const i1 = cc::bit_cast<u32>(y1) >> 31;
    auto const i2 = cc::bit_cast<u32>(y2) >> 30;
    auto const i3 = cc::bit_cast<u32>(y3) >> 29;
    auto const shift = (i3 & 4) | (((i2 & 2) | (i1 & ~u32(2))) & ~u32(4));
    return (u32(0x2e74) >> shift) & 0x0101;
}

/// slug_solve_horizontal when `vertical` is false, slug_solve_vertical with the axes swapped when it is true.
[[nodiscard]] tg::vec2f solve(texel4 p12, f32 p3x, f32 p3y, bool vertical)
{
    // In the horizontal solve `u` is x and `v` is y; the vertical one swaps them.
    auto const u1 = vertical ? p12.y : p12.x;
    auto const v1 = vertical ? p12.x : p12.y;
    auto const u2 = vertical ? p12.w : p12.z;
    auto const v2 = vertical ? p12.z : p12.w;
    auto const u3 = vertical ? p3y : p3x;
    auto const v3 = vertical ? p3x : p3y;

    auto const au = u1 - u2 * 2.0f + u3;
    auto const av = v1 - v2 * 2.0f + v3;
    auto const bu = u1 - u2;
    auto const bv = v1 - v2;
    auto t1 = 0.0f;
    auto t2 = 0.0f;
    if (tg::abs(av) < 1.0f / 65536.0f)
    {
        t1 = v1 * (0.5f / bv);
        t2 = t1;
    }
    else
    {
        auto const ra = 1.0f / av;
        auto const d = tg::sqrt(cc::max(bv * bv - av * v1, 0.0f));
        t1 = (bv - d) * ra;
        t2 = (bv + d) * ra;
    }
    return tg::vec2f((au * t1 - bu * 2.0f) * t1 + u1, (au * t2 - bu * 2.0f) * t2 + u1);
}
} // namespace

f32 slug_reference_coverage(slug_atlas const& atlas,
                            slug_instance const& instance,
                            tg::pos2f em,
                            tg::vec2f em_per_pixel,
                            bool weight_boost)
{
    auto const glyph_x = int(instance.glyph_location & 0xffff);
    auto const glyph_y = int(instance.glyph_location >> 16);
    auto const band_max_x = int(instance.band_info & 0xffff);
    auto const glyph_w = int(instance.band_info >> 16);
    auto const band_max_y = glyph_w & 0xff;
    auto const& banding = instance.banding;

    auto const ppe_x = 1.0f / em_per_pixel[0];
    auto const ppe_y = 1.0f / em_per_pixel[1];
    auto const band_x = cc::clamp(int(em[0] * banding[0] + banding[2]), 0, band_max_x);
    auto const band_y = cc::clamp(int(em[1] * banding[1] + banding[3]), 0, band_max_y);

    auto const location = [&](u32 offset)
    {
        auto const x = glyph_x + int(offset);
        return tg::pos2i(x & 4095, glyph_y + (x >> 12));
    };

    auto xcov = 0.0f;
    auto xwgt = 0.0f;
    auto const hband = band_at(atlas, glyph_x + band_y, glyph_y);
    auto const hloc = location(hband.y);
    for (auto i = 0; i < int(hband.x); ++i)
    {
        auto const entry = band_at(atlas, hloc[0] + i, hloc[1]);
        auto const a = curve_at(atlas, int(entry.x), int(entry.y));
        auto const b = curve_at(atlas, int(entry.x) + 1, int(entry.y));
        auto const p12 = texel4{a.x - em[0], a.y - em[1], a.z - em[0], a.w - em[1]};
        auto const p3x = b.x - em[0];
        auto const p3y = b.y - em[1];
        if (cc::max(cc::max(p12.x, p12.z), p3x) * ppe_x < -0.5f)
            break;
        auto const code = root_code(p12.y, p12.w, p3y);
        if (code != 0)
        {
            auto const r = solve(p12, p3x, p3y, false) * ppe_x;
            if ((code & 1) != 0)
            {
                xcov += saturate(r[0] + 0.5f);
                xwgt = cc::max(xwgt, saturate(1.0f - tg::abs(r[0]) * 2.0f));
            }
            if (code > 1)
            {
                xcov -= saturate(r[1] + 0.5f);
                xwgt = cc::max(xwgt, saturate(1.0f - tg::abs(r[1]) * 2.0f));
            }
        }
    }

    auto ycov = 0.0f;
    auto ywgt = 0.0f;
    auto const vband = band_at(atlas, glyph_x + band_max_y + 1 + band_x, glyph_y);
    auto const vloc = location(vband.y);
    for (auto i = 0; i < int(vband.x); ++i)
    {
        auto const entry = band_at(atlas, vloc[0] + i, vloc[1]);
        auto const a = curve_at(atlas, int(entry.x), int(entry.y));
        auto const b = curve_at(atlas, int(entry.x) + 1, int(entry.y));
        auto const p12 = texel4{a.x - em[0], a.y - em[1], a.z - em[0], a.w - em[1]};
        auto const p3x = b.x - em[0];
        auto const p3y = b.y - em[1];
        if (cc::max(cc::max(p12.y, p12.w), p3y) * ppe_y < -0.5f)
            break;
        auto const code = root_code(p12.x, p12.z, p3x);
        if (code != 0)
        {
            auto const r = solve(p12, p3x, p3y, true) * ppe_y;
            if ((code & 1) != 0)
            {
                ycov -= saturate(r[0] + 0.5f);
                ywgt = cc::max(ywgt, saturate(1.0f - tg::abs(r[0]) * 2.0f));
            }
            if (code > 1)
            {
                ycov += saturate(r[1] + 0.5f);
                ywgt = cc::max(ywgt, saturate(1.0f - tg::abs(r[1]) * 2.0f));
            }
        }
    }

    auto const raw = cc::max(tg::abs(xcov * xwgt + ycov * ywgt) / cc::max(xwgt + ywgt, 1.0f / 65536.0f),
                             cc::min(tg::abs(xcov), tg::abs(ycov)));
    auto filled = 0.0f;
    if ((glyph_w & 0x1000) != 0)
    {
        auto const half = raw * 0.5f;
        filled = 1.0f - tg::abs(1.0f - (half - tg::floor(half)) * 2.0f);
    }
    else
        filled = saturate(raw);
    return weight_boost ? tg::sqrt(filled) : filled;
}
} // namespace sr::impl
