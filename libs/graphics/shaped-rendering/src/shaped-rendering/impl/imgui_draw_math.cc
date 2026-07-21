#include <clean-core/common/asserts.hh>
#include <shaped-rendering/impl/imgui_draw_math.hh>

#include <cstring> // memcpy

namespace sr::impl
{
imgui_ortho_constants compute_ortho_constants(tg::pos2f display_pos, tg::vec2f display_size)
{
    auto const left = display_pos[0];
    auto const right = display_pos[0] + display_size[0];
    auto const top = display_pos[1];
    auto const bottom = display_pos[1] + display_size[1];

    CC_ASSERT(right != left && bottom != top, "imgui display size must be non-degenerate");

    return {.scale = tg::vec2f(2.0f / (right - left), 2.0f / (top - bottom)),
            .translate = tg::vec2f((right + left) / (left - right), (top + bottom) / (bottom - top))};
}

cc::optional<tg::aabb2i> compute_scissor(tg::aabb2f const& clip_rect,
                                         tg::pos2f display_pos,
                                         tg::vec2f framebuffer_scale,
                                         tg::vec2i target_size)
{
    auto const to_framebuffer = [&](tg::pos2f p)
    {
        return tg::pos2i(int((p[0] - display_pos[0]) * framebuffer_scale[0]),
                         int((p[1] - display_pos[1]) * framebuffer_scale[1]));
    };

    auto const raw_min = to_framebuffer(clip_rect.min);
    auto const raw_max = to_framebuffer(clip_rect.max);

    // Clamp into the target. imgui hands out rects that poke outside it (a window dragged off-screen),
    // and both tier-1 backends reject an out-of-bounds scissor outright.
    auto const clamped = tg::aabb2i(tg::pos2i(raw_min[0] < 0 ? 0 : raw_min[0], raw_min[1] < 0 ? 0 : raw_min[1]),
                                    tg::pos2i(raw_max[0] > target_size[0] ? target_size[0] : raw_max[0],
                                              raw_max[1] > target_size[1] ? target_size[1] : raw_max[1]));

    if (clamped.max[0] <= clamped.min[0] || clamped.max[1] <= clamped.min[1])
        return {};

    return clamped;
}

cc::pinned_data<cc::byte const> pack_texture_rect(cc::byte const* pixels,
                                                  isize pitch,
                                                  isize bytes_per_pixel,
                                                  tg::pos2i offset,
                                                  tg::vec2i size)
{
    CC_ASSERT(pixels != nullptr, "source pixels must not be null");
    CC_ASSERT(size[0] > 0 && size[1] > 0, "rect must be non-empty");
    CC_ASSERT(offset[0] >= 0 && offset[1] >= 0, "rect offset must be non-negative");
    CC_ASSERT(bytes_per_pixel > 0 && pitch >= (offset[0] + size[0]) * bytes_per_pixel, "rect must fit the pitch");

    auto const row_bytes = isize(size[0]) * bytes_per_pixel;
    auto packed = cc::pinned_data<cc::byte>::create_uninitialized(row_bytes * isize(size[1]));

    for (auto y = 0; y < size[1]; ++y)
    {
        auto const* const src = pixels + (isize(offset[1]) + y) * pitch + isize(offset[0]) * bytes_per_pixel;
        std::memcpy(packed.data() + isize(y) * row_bytes, src, size_t(row_bytes));
    }

    return packed;
}
} // namespace sr::impl
