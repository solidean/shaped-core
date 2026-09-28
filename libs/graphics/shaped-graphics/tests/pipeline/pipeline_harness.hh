#pragma once

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/command_list/raster.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <typed-geometry/linalg/vec.hh>

// The tier-1 pipeline tests' harness: every test reads as "set up, draw, expect".
// libs/graphics/shaped-graphics/docs/tier1-pipeline-tests.md is the plan these helpers serve.
//
// **Pixel coordinates put row 0 at the top**, which is clip-space y = +1 on every backend.
// A rect whose edges lie on pixel boundaries covers exactly the pixels inside it, since a pixel is sampled at its centre.
// That is what lets every oracle here be exact rather than a tolerance.

namespace sg_test
{
/// The targets `draw_offscreen` creates, and what each starts as.
struct offscreen
{
    int width = 0;
    int height = 1;
    cc::fixed_vector<sg::pixel_format, sg::max_color_targets> colors;
    cc::optional<sg::pixel_format> depth_stencil;

    /// The generated target set's name, which a pipeline built for it is checked against.
    cc::string_view target_set;

    tg::vec4f clear_color = tg::vec4f(0, 0, 0, 0);
    float clear_depth = 1.0f;
    cc::u8 clear_stencil = 0;
};

/// One target read back, rows top to bottom and tightly packed.
struct target_pixels
{
    sg::pixel_format format = sg::pixel_format::undefined;
    int width = 0;
    int height = 0;
    cc::vector<cc::byte> bytes;

    /// The texel at (x, y) as `T`, which must be the texel's size.
    template <class T>
    [[nodiscard]] T at(int x, int y) const
    {
        CC_ASSERT(cc::isize(sizeof(T)) * width * height == bytes.size(), "T is not this target's texel size");
        CC_ASSERT(x >= 0 && x < width && y >= 0 && y < height, "the texel is outside the target");
        auto value = T();
        cc::memcpy(&value, bytes.data() + (cc::isize(y) * width + x) * cc::isize(sizeof(T)), sizeof(T));
        return value;
    }

    /// An rgba8 texel's channels, 0 to 255.
    [[nodiscard]] tg::vec4i rgba8(int x, int y) const;

    /// An r32_float, rg32_float, rgba32_float or rgba16_float texel as floats; missing channels are 0.
    [[nodiscard]] tg::vec4f rgba_float(int x, int y) const;
};

/// Every color target of one `draw_offscreen`, in the order `offscreen::colors` named them.
struct offscreen_pixels
{
    cc::vector<target_pixels> colors;

    [[nodiscard]] target_pixels const& operator[](cc::isize i) const { return colors[i]; }
};

/// The targets a recording draws into, for a test that opens more than one rendering scope.
struct offscreen_targets
{
    cc::vector<sg::raw_texture_handle> colors;
    sg::raw_texture_handle depth_stencil;
    offscreen const* description = nullptr;

    /// Each target cleared to the description's values, which is the first scope's usual start.
    [[nodiscard]] sg::rendering_info cleared() const;
    /// Each target kept as the previous scope left it.
    [[nodiscard]] sg::rendering_info preserved() const;
};

/// Creates the described targets, records `record` into one command list, submits it, and reads every color target back.
/// `record` opens its own rendering scopes, so a test may draw into the targets more than once.
/// It runs before this returns, so it may capture by reference.
/// The targets are persistent and dropped once read.
[[nodiscard]] cc::shared_async<offscreen_pixels> draw_offscreen_passes(
    sg::context& ctx,
    offscreen desc,
    cc::function_ref<void(sg::command_list&, offscreen_targets const&)> record);

/// The common case: one scope over the cleared targets, and `record` draws into it.
[[nodiscard]] cc::shared_async<offscreen_pixels> draw_offscreen(sg::context& ctx,
                                                                offscreen desc,
                                                                cc::function_ref<void(sg::rendering_scope&)> record);

/// The clip-space rect `(left, bottom, right, top)` covering exactly the pixels `[x0, x1) × [y0, y1)` of a `width` × `height` target.
[[nodiscard]] tg::vec4f clip_rect(int x0, int y0, int x1, int y1, int width, int height);

/// A single pixel's clip rect.
[[nodiscard]] inline tg::vec4f clip_pixel(int x, int y, int width, int height)
{
    return clip_rect(x, y, x + 1, y + 1, width, height);
}

/// An IEEE half's bits as a float.
[[nodiscard]] float half_to_float(cc::u16 bits);
} // namespace sg_test
