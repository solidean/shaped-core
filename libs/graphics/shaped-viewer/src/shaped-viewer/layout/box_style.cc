#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::max, cc::min
#include <shaped-viewer/layout/box_style.hh>

namespace sv
{
tg::aabb2i inset(tg::aabb2i rect, box_insets i)
{
    auto const x0 = rect.min[0] + i.left;
    auto const y0 = rect.min[1] + i.top;
    auto const x1 = cc::max(rect.max[0] - i.right, x0);
    auto const y1 = cc::max(rect.max[1] - i.bottom, y0);
    return tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(x1, y1));
}

tg::aabb2i inset(tg::aabb2i rect, int v)
{
    return inset(rect, box_insets::all(v));
}

tg::aabb2i border_box(tg::aabb2i rect, box_style const& s)
{
    return inset(rect, s.margin);
}

tg::aabb2i content_box(tg::aabb2i rect, box_style const& s)
{
    return inset(inset(border_box(rect, s), s.border), s.padding);
}

int border_bands(tg::aabb2i rect, int width, cc::span<tg::aabb2i> out)
{
    CC_ASSERT(out.size() >= 4, "border_bands writes up to four bands");

    if (width <= 0)
        return 0;

    auto const x0 = rect.min[0];
    auto const y0 = rect.min[1];
    auto const x1 = rect.max[0];
    auto const y1 = rect.max[1];
    if (x1 <= x0 || y1 <= y0)
        return 0;

    // The horizontal bands span the full width, so the vertical ones only cover what is left between them.
    // Clamping each edge against the opposite one is what collapses an over-thick ring into a single filled band.
    auto const top = cc::min(y0 + width, y1);
    auto const bottom = cc::max(y1 - width, top);
    auto const left = cc::min(x0 + width, x1);
    auto const right = cc::max(x1 - width, left);

    auto count = 0;
    out[count++] = tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(x1, top));
    if (bottom < y1)
        out[count++] = tg::aabb2i(tg::pos2i(x0, bottom), tg::pos2i(x1, y1));

    if (top < bottom)
    {
        out[count++] = tg::aabb2i(tg::pos2i(x0, top), tg::pos2i(left, bottom));
        if (right < x1)
            out[count++] = tg::aabb2i(tg::pos2i(right, top), tg::pos2i(x1, bottom));
    }

    return count;
}
} // namespace sv
