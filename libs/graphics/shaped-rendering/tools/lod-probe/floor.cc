#include "floor.hh"

#include "probe.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <lod_probe_shaders.hh>
#include <shaped-graphics/all.hh>
#include <typed-geometry/camera/camera.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/compose.hh>

namespace lod_probe
{
namespace
{
using floor_vertex = shaders::floor::floor_vertex;

constexpr auto image_size = tg::vec2i(960, 540);
constexpr int detail_size = 1024;
constexpr int detail_mips = 11;

/// The level the heatmap's last colour stands for; the floor seldom reaches past it.
constexpr float heatmap_top = 8.0f;

/// What the colour render samples: the page, the page with each mip's ink in its own colour, or one flat colour per mip.
enum class detail_kind
{
    page,
    tinted,
    mip_colors,
};

/// One render: the floor when `page_lod` < 0, else the page flat on screen with a footprint of level `page_lod`.
/// `floor_repeat` is how many floor units one repeat of the texture spans, so a larger one reads lower levels.
struct view_setting
{
    float page_lod;
    int anisotropy;
    float sampler_bias;
    detail_kind detail = detail_kind::page;
    float floor_repeat = 1.0f;
};

// On the floor, the setting a renderer with a negative bias for an upscaler would use first, then the two that isolate it.
// The pages put the whole frame a quarter past a level, where NVIDIA's blends differ most at 16x: dx12 still reads the
// finer mip alone while vulkan is half way to the next.
// They take no bias, because a negative one samples finer than the screen resolves and its aliasing hides the blend.
constexpr view_setting view_settings[] = {
    {.page_lod = -1.0f, .anisotropy = 16, .sampler_bias = -1.0f},
    {.page_lod = -1.0f, .anisotropy = 16, .sampler_bias = 0.0f},
    {.page_lod = -1.0f, .anisotropy = 1, .sampler_bias = -1.0f},
    {.page_lod = 1.25f, .anisotropy = 16, .sampler_bias = 0.0f},
    {.page_lod = 1.25f, .anisotropy = 16, .sampler_bias = 0.0f, .detail = detail_kind::tinted},
    // Levels 0 to 5 over the frame, each mip its own colour: a band per mip, and where two blend.
    {.page_lod = -1.0f, .anisotropy = 16, .sampler_bias = 0.0f, .detail = detail_kind::mip_colors, .floor_repeat = 4.0f},
};

[[nodiscard]] tg::mat4f floor_view_projection()
{
    auto const aspect = float(image_size[0]) / float(image_size[1]);
    auto const projection = tg::perspective_lh_zero_to_one(tg::angle_f::make_from_degree(50.0f), aspect, 0.05f, 500.0f);
    auto const eye = tg::pos3f(0.0f, 2.0f, 0.0f);
    return tg::compose(projection, tg::look_at(eye, tg::pos3f(0.0f, 0.0f, 7.0f))).to_mat();
}

/// The floor: one quad from below the camera to far beyond the horizon's resolution, one texture repeat per `units_per_repeat`.
/// At one unit, most of the frame sits between levels 1 and 6, where both mips of a blend are real detail.
[[nodiscard]] cc::vector<floor_vertex> floor_vertices(float units_per_repeat)
{
    constexpr float half_width = 80.0f;
    constexpr float near_z = -2.0f;
    constexpr float far_z = 300.0f;
    auto out = cc::vector<floor_vertex>();
    for (auto const corner : {tg::vec2f(-half_width, near_z), tg::vec2f(half_width, near_z),
                              tg::vec2f(half_width, far_z), tg::vec2f(-half_width, far_z)})
        out.push_back({.position = tg::pos3f(corner[0], 0.0f, corner[1]),
                       .uv = tg::vec2f(corner[0], -corner[1]) / units_per_repeat});
    return out;
}

/// The page flat on screen, covering the frame, with every pixel's footprint 2^`lod` texels on a side.
/// Its positions are already in clip space, so it is drawn with an identity view-projection.
[[nodiscard]] cc::vector<floor_vertex> page_vertices(float lod)
{
    auto const texels_per_pixel = tg::pow(2.0f, lod);
    auto const extent = tg::vec2f(float(image_size[0]), float(image_size[1])) * texels_per_pixel / float(detail_size);
    auto out = cc::vector<floor_vertex>();
    for (auto const corner : {tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(1, 1), tg::vec2f(0, 1)})
        out.push_back({.position = tg::pos3f(2.0f * corner[0] - 1.0f, 1.0f - 2.0f * corner[1], 0.5f),
                       .uv = tg::vec2f(corner[0] * extent[0], corner[1] * extent[1])});
    return out;
}

// A 5x7 font, one row per byte with the leftmost column in bit 4; enough for the page's text.
constexpr char glyph_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-";
constexpr u8 glyph_rows[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 17, 10, 4, 4, 4},    {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {31, 2, 4, 2, 1, 17, 14},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14},   {6, 8, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {0, 0, 0, 0, 0, 12, 12},      {0, 0, 0, 31, 0, 0, 0},
};

[[nodiscard]] u8 const* glyph_of(char c)
{
    for (auto i = 0; glyph_chars[i] != 0; ++i)
        if (glyph_chars[i] == c)
            return glyph_rows[i];
    return nullptr; // a space, or anything the font lacks
}

/// A page of text in shrinking sizes, like an eye chart: dark ink on paper, from 8 texels per font pixel down to 1.
/// Text is what makes a quarter level visible, because a reader judges sharpness by whether the letters still read.
[[nodiscard]] cc::vector<u8> detail_mip0()
{
    constexpr u8 paper[3] = {242, 238, 228};
    constexpr u8 ink[3] = {28, 32, 44};
    constexpr int scales[] = {8, 6, 5, 4, 4, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1};
    constexpr cc::string_view lines[] = {
        "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789",
        "PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS - SPHINX OF BLACK QUARTZ JUDGE MY VOW",
        "HOW VEXINGLY QUICK DAFT ZEBRAS JUMP. 2468 1357 MIP LEVEL OF DETAIL",
    };
    auto out = cc::vector<u8>::create_defaulted(isize(detail_size) * detail_size * 4);
    for (auto i = isize(0); i < isize(detail_size) * detail_size; ++i)
    {
        for (auto c = 0; c < 3; ++c)
            out[i * 4 + c] = paper[c];
        out[i * 4 + 3] = 255;
    }
    auto line_index = 0;
    for (auto y0 = 4; y0 < detail_size;)
        for (auto const s : scales)
        {
            if (y0 + 7 * s > detail_size)
            {
                y0 = detail_size;
                break;
            }
            auto const text = lines[line_index++ % 3];
            for (auto ci = 0, x0 = 2 * s; ci < int(text.size()) && x0 + 5 * s <= detail_size; ++ci, x0 += 6 * s)
            {
                auto const* const rows = glyph_of(text[ci]);
                if (rows == nullptr)
                    continue;
                for (auto gy = 0; gy < 7 * s; ++gy)
                    for (auto gx = 0; gx < 5 * s; ++gx)
                        if ((rows[gy / s] >> (4 - gx / s)) & 1)
                            for (auto c = 0; c < 3; ++c)
                                out[(isize(y0 + gy) * detail_size + x0 + gx) * 4 + c] = ink[c];
            }
            y0 += 9 * s;
        }
    return out;
}

/// The next mip: each texel the mean of the four below it.
[[nodiscard]] cc::vector<u8> halve(cc::span<u8 const> src, int src_size)
{
    auto const size = src_size / 2;
    auto out = cc::vector<u8>::create_defaulted(isize(size) * size * 4);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            for (auto c = 0; c < 4; ++c)
            {
                auto const at
                    = [&](int dx, int dy) { return int(src[(isize(2 * y + dy) * src_size + 2 * x + dx) * 4 + c]); };
                out[(isize(y) * size + x) * 4 + c] = u8((at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1) + 2) / 4);
            }
    return out;
}

/// `level` with its ink recoloured to `mip`'s colour, so a render shows which mips it blended: black, blue, red, green, ...
[[nodiscard]] cc::vector<u8> tint_ink(cc::span<u8 const> level, int mip)
{
    constexpr u8 tints[][3]
        = {{28, 32, 44}, {30, 90, 225}, {215, 40, 40}, {30, 150, 60}, {230, 130, 0}, {140, 50, 180}};
    constexpr float paper = 242.0f;
    constexpr float ink = 28.0f;
    auto const* const tint = tints[cc::min(mip, int(sizeof(tints) / sizeof(tints[0])) - 1)];
    auto out = cc::vector<u8>::create_copy_of(level);
    for (auto i = isize(0); i < isize(out.size()); i += 4)
    {
        auto const darkness = cc::clamp((paper - float(level[i])) / (paper - ink), 0.0f, 1.0f);
        for (auto c = 0; c < 3; ++c)
            out[i + c] = u8(float(level[i + c]) + darkness * (float(tint[c]) - float(level[i + c])) + 0.5f);
    }
    return out;
}

/// Mip `mip` of the mip-colour texture: its own colour, in an 8x8 checker of two shades that keeps its texture-space size on every mip.
[[nodiscard]] cc::vector<u8> mip_color_level(int mip)
{
    constexpr u8 colors[][3] = {{235, 235, 235}, {40, 110, 230}, {225, 45, 45},  {40, 170, 70},
                                {245, 165, 20},  {150, 60, 200}, {30, 190, 200}, {240, 90, 170}};
    auto const* const color = colors[cc::min(mip, int(sizeof(colors) / sizeof(colors[0])) - 1)];
    auto const size = detail_size >> mip;
    auto const square = cc::max(1, size / 8);
    auto out = cc::vector<u8>::create_defaulted(isize(size) * size * 4);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const shade = ((x / square + y / square) % 2 == 0 || size < 8) ? 1.0f : 0.82f;
            auto* const p = &out[(isize(y) * size + x) * 4];
            for (auto c = 0; c < 3; ++c)
                p[c] = u8(float(color[c]) * shade + 0.5f);
            p[3] = 255;
        }
    return out;
}

/// The page with every mip box-filtered from the one below it, its ink recoloured per mip for `tinted` (see tint_ink),
/// or for `mip_colors` no page at all but mip_color_level.
[[nodiscard]] sg::texture_2d create_detail(sg::context& ctx, detail_kind kind)
{
    auto const detail = ctx.persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = detail_size,
        .height = detail_size,
        .mip_levels = detail_mips,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst,
    });
    auto cmd = ctx.create_command_list();
    auto level = detail_mip0();
    for (auto mip = 0; mip < detail_mips; ++mip)
    {
        auto const uploaded = kind == detail_kind::tinted ? tint_ink(level, mip)
                            : kind == detail_kind::mip_colors
                                ? mip_color_level(mip)
                                : cc::vector<u8>::create_copy_of(cc::span<u8 const>(level));
        cmd->upload.bytes_to_texture(detail.raw(), cc::as_bytes(cc::span<u8 const>(uploaded)),
                                     {.mip_level = mip, .array_layer = 0, .aspect = sg::texture_aspect::color});
        if (mip + 1 < detail_mips)
            level = halve(level, detail_size >> mip);
    }
    ctx.submit_command_list(cc::move(cmd));
    return detail;
}

// Google's Turbo colormap, as its published polynomial fit; t in [0, 1].
[[nodiscard]] tg::vec3f turbo(float t)
{
    t = cc::clamp(t, 0.0f, 1.0f);
    auto const r
        = 0.13572138f
        + t * (4.61539260f + t * (-42.66032258f + t * (132.13108234f + t * (-152.94239396f + t * 59.28637943f))));
    auto const g = 0.09140261f
                 + t * (2.19418839f + t * (4.84296658f + t * (-14.18503333f + t * (4.27729857f + t * 2.82956604f))));
    auto const b
        = 0.10667330f
        + t * (12.64194608f + t * (-60.58204836f + t * (110.36276771f + t * (-89.90310912f + t * 27.34824973f))));
    return tg::vec3f(cc::clamp(r, 0.0f, 1.0f), cc::clamp(g, 0.0f, 1.0f), cc::clamp(b, 0.0f, 1.0f));
}

/// An RGB8 image, row-major, for the panels and the figure.
struct rgb_image
{
    tg::vec2i size;
    cc::vector<u8> pixels;

    static rgb_image make(tg::vec2i size)
    {
        return {size, cc::vector<u8>::create_defaulted(isize(size[0]) * size[1] * 3)};
    }

    void set(int x, int y, tg::vec3f c)
    {
        auto* const p = &pixels[(isize(y) * size[0] + x) * 3];
        for (auto i = 0; i < 3; ++i)
            p[i] = u8(cc::clamp(c[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    void blit(rgb_image const& src, int x0, int y0)
    {
        for (auto y = 0; y < src.size[1]; ++y)
            for (auto x = 0; x < src.size[0]; ++x)
                for (auto i = 0; i < 3; ++i)
                    pixels[(isize(y0 + y) * size[0] + x0 + x) * 3 + i] = src.pixels[(isize(y) * src.size[0] + x) * 3 + i];
    }
};

[[nodiscard]] bool is_floor(float lod)
{
    return lod >= 0.0f;
}

[[nodiscard]] rgb_image color_panel(floor_view const& v, tg::vec2i size)
{
    auto img = rgb_image::make(size);
    for (auto i = isize(0); i < isize(size[0]) * size[1]; ++i)
        for (auto c = 0; c < 3; ++c)
            img.pixels[i * 3 + c] = is_floor(v.lod[i]) ? v.rgba[i * 4 + c] : u8(24);
    return img;
}

[[nodiscard]] rgb_image heatmap_panel(cc::span<float const> lod, tg::vec2i size)
{
    auto img = rgb_image::make(size);
    for (auto y = 0; y < size[1]; ++y)
        for (auto x = 0; x < size[0]; ++x)
        {
            auto const l = lod[isize(y) * size[0] + x];
            img.set(x, y, is_floor(l) ? turbo(l / heatmap_top) : tg::vec3f(0.1f, 0.1f, 0.1f));
        }
    return img;
}

/// Whether a contour passes between this pixel and its right or lower neighbour: one per level, where the sample crosses m + 0.5.
/// That is where any symmetric blend is half way, so two maps' contours sit apart by exactly their level gap.
[[nodiscard]] bool on_contour(cc::span<float const> lod, tg::vec2i size, int x, int y)
{
    auto const band = [&](int px, int py) { return int(tg::floor(lod[isize(py) * size[0] + px] + 0.5f)); };
    if (x + 1 >= size[0] || y + 1 >= size[1])
        return false;
    auto const l = lod[isize(y) * size[0] + x];
    if (!is_floor(l) || !is_floor(lod[isize(y) * size[0] + x + 1]) || !is_floor(lod[isize(y + 1) * size[0] + x]))
        return false;
    return band(x, y) != band(x + 1, y) || band(x, y) != band(x, y + 1);
}

/// Draws `lod`'s contours onto `img` in `color`, two pixels wide so they survive a downscaled view.
void draw_contours(rgb_image& img, cc::span<float const> lod, tg::vec3f color)
{
    for (auto y = 0; y < img.size[1]; ++y)
        for (auto x = 0; x < img.size[0]; ++x)
            if (on_contour(lod, img.size, x, y))
            {
                img.set(x, y, color);
                if (y + 1 < img.size[1])
                    img.set(x, y + 1, color);
            }
}

/// A level map with its own contours in white, or in black for a standalone image.
[[nodiscard]] rgb_image heatmap_with_contours(cc::span<float const> lod,
                                              tg::vec2i size,
                                              tg::vec3f color = tg::vec3f(1, 1, 1))
{
    auto img = heatmap_panel(lod, size);
    draw_contours(img, lod, color);
    return img;
}

/// The first map, dimmed, with its contours in white and the second's in black: a shift between them is the gap.
[[nodiscard]] rgb_image contour_panel(cc::span<float const> a, cc::span<float const> b, tg::vec2i size)
{
    auto img = heatmap_panel(a, size);
    for (auto i = isize(0); i < isize(img.pixels.size()); ++i)
        img.pixels[i] = u8(int(img.pixels[i]) * 3 / 5);
    draw_contours(img, b, tg::vec3f(0, 0, 0));
    draw_contours(img, a, tg::vec3f(1, 1, 1));
    return img;
}

/// |a - b| of the two renders, eight times over: where the two backends filtered alike, black.
[[nodiscard]] rgb_image color_difference_panel(floor_view const& a, floor_view const& b, tg::vec2i size)
{
    auto img = rgb_image::make(size);
    for (auto i = isize(0); i < isize(size[0]) * size[1]; ++i)
        for (auto c = 0; c < 3; ++c)
            img.pixels[i * 3 + c] = u8(cc::min(255, 8 * tg::abs(int(a.rgba[i * 4 + c]) - int(b.rgba[i * 4 + c]))));
    return img;
}

[[nodiscard]] cc::result<cc::unit> write_png(cc::string_view path, rgb_image const& img)
{
    auto const out_image
        = babel::image::image{.width = img.size[0],
                              .height = img.size[1],
                              .channels = 3,
                              .pixels = cc::vector<byte>::create_copy_of(cc::as_bytes(cc::span<u8 const>(img.pixels)))};
    auto adapter = cc::file_write_stream_adapter::create(path);
    CC_RETURN_IF_ERROR(adapter);
    cc::write_stream out = adapter.value();
    CC_RETURN_IF_ERROR(babel::image::write(out, out_image, babel::image::format::png, {}));
    CC_RETURN_IF_ERROR(out.flush());
    return cc::unit{};
}

[[nodiscard]] cc::string view_name(floor_view const& v)
{
    return cc::format("{}-aniso{}-bias{}", v.scene, v.anisotropy, v.sampler_bias);
}
} // namespace

cc::shared_async<cc::result<floor_capture>> capture_floor(sg::context_handle ctx)
{
    auto const building_lod
        = ctx->cached.acquire_raster_pipeline(shaders::floor::floor_lod, {.lod = sg::pixel_format::r32_float});
    auto const building_color
        = ctx->cached.acquire_raster_pipeline(shaders::floor::floor_color, {.color = sg::pixel_format::rgba8_unorm});
    co_await cc::async_settled(building_lod);
    co_await cc::async_settled(building_color);
    if (building_lod->try_value() == nullptr)
        co_return cc::error(pipeline_error(building_lod));
    if (building_color->try_value() == nullptr)
        co_return cc::error(pipeline_error(building_color));
    auto const lod_pipeline = *building_lod->try_value();
    auto const color_pipeline = *building_color->try_value();
    auto const lod_layout = ctx->cached.acquire_binding_group_layout<shaders::floor::lod_source>();
    auto const color_layout = ctx->cached.acquire_binding_group_layout<shaders::floor::color_source>();

    // Every view's two readbacks land in one epoch; the new budget applies from the next.
    auto const per_view = isize(image_size[0]) * image_size[1] * (isize(sizeof(float)) + 4);
    ctx->download.set_budget(isize(sizeof(view_settings) / sizeof(view_settings[0])) * per_view + (isize(1) << 20));
    // The three detail textures and the pyramid upload in that epoch too, 4 MB per mip 0 and a third more for the chain.
    ctx->upload.set_inline_budget(isize(32) << 20);
    ctx->advance_epoch();

    auto const pyramid = create_pyramid(*ctx);
    sg::texture_2d const details[] = {
        create_detail(*ctx, detail_kind::page),
        create_detail(*ctx, detail_kind::tinted),
        create_detail(*ctx, detail_kind::mip_colors),
    };
    auto const indices
        = ctx->persistent.create_buffer_from_data(cc::vector<u16>{0, 1, 2, 0, 2, 3}, sg::buffer_usage::index_buffer);

    auto out = floor_capture{.backend = cc::string(ctx->backend() == sg::backend_kind::dx12 ? "dx12" : "vulkan"),
                             .size = image_size};
    for (auto const& setting : view_settings)
    {
        auto const is_page = setting.page_lod >= 0.0f;
        auto const vertices = ctx->persistent.create_buffer_from_data(
            is_page ? page_vertices(setting.page_lod) : floor_vertices(setting.floor_repeat),
            sg::buffer_usage::vertex_buffer);
        auto const constants = shaders::floor::floor_constants{.view_projection = is_page ? tg::mat4f::identity
                                                                                          : floor_view_projection()};
        auto const smp = probe_sampler(setting.anisotropy, setting.sampler_bias);
        auto const target_of = [&](sg::pixel_format f)
        {
            return ctx->persistent.create_texture_2d({
                .format = f,
                .width = image_size[0],
                .height = image_size[1],
                .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
            });
        };
        auto const lod_rt = target_of(sg::pixel_format::r32_float);
        auto const color_rt = target_of(sg::pixel_format::rgba8_unorm);

        auto cmd = ctx->create_command_list();
        {
            auto const group = ctx->transient.create_binding_group(
                *cmd, lod_layout, shaders::floor::lod_source{.pyramid = pyramid.as_texture_view(), .smp = smp});
            auto scope = cmd->raster.render_to(
                shaders::floor::lod_out{.lod = lod_rt.as_render_target_view().cleared(tg::vec4f(-1, 0, 0, 0))});
            scope.bind_pipeline(*lod_pipeline);
            scope.bind_group(0, *group);
            scope.bind_vertex_buffers({vertices.as_vertex_buffer()});
            scope.bind_index_buffer(indices.as_index_buffer());
            scope.set_inline_constants(constants.to_block());
            scope.draw_indexed({.index_range = {.offset = 0, .size = 6}});
        }
        {
            auto const group = ctx->transient.create_binding_group(
                *cmd, color_layout,
                shaders::floor::color_source{.detail = details[int(setting.detail)].as_texture_view(), .smp = smp});
            auto scope = cmd->raster.render_to(shaders::floor::color_out{
                .color = color_rt.as_render_target_view().cleared(tg::vec4f(0.1f, 0.1f, 0.1f, 1))});
            scope.bind_pipeline(*color_pipeline);
            scope.bind_group(0, *group);
            scope.bind_vertex_buffers({vertices.as_vertex_buffer()});
            scope.bind_index_buffer(indices.as_index_buffer());
            scope.set_inline_constants(constants.to_block());
            scope.draw_indexed({.index_range = {.offset = 0, .size = 6}});
        }
        auto const lod_back = cmd->download.bytes_from_texture(lod_rt.raw());
        auto const color_back = cmd->download.bytes_from_texture(color_rt.raw());
        ctx->submit_command_list(cc::move(cmd));

        auto const scene = is_page ? cc::format("{}-lod{}", setting.detail == detail_kind::tinted ? "tinted" : "page",
                                                setting.page_lod)
                         : setting.detail == detail_kind::mip_colors ? cc::string("mip-colors")
                                                                     : cc::string("floor");
        auto view = floor_view{.scene = scene, .anisotropy = setting.anisotropy, .sampler_bias = setting.sampler_bias};
        {
            auto const landing = lod_back.bytes();
            auto const& bytes = co_await landing;
            auto const floats = cc::span<float const>(reinterpret_cast<float const*>(bytes.span().data()),
                                                      bytes.span().size() / isize(sizeof(float)));
            view.lod = cc::vector<float>::create_copy_of(floats);
        }
        {
            auto const landing = color_back.bytes();
            auto const& bytes = co_await landing;
            view.rgba = cc::vector<u8>::create_copy_of(
                cc::span<u8 const>(reinterpret_cast<u8 const*>(bytes.span().data()), bytes.span().size()));
        }
        out.views.push_back(cc::move(view));
    }
    co_return out;
}

cc::result<cc::vector<cc::string>> write_floor_images(cc::span<floor_capture const> captures, cc::string_view base)
{
    auto written = cc::vector<cc::string>();
    for (auto const& c : captures)
        for (auto const& v : c.views)
        {
            auto const color_path = cc::format("{}-{}-{}.png", base, view_name(v), c.backend);
            CC_RETURN_IF_ERROR(write_png(color_path, color_panel(v, c.size)));
            written.push_back(color_path);
            auto const lod_path = cc::format("{}-{}-{}-lod.png", base, view_name(v), c.backend);
            CC_RETURN_IF_ERROR(write_png(lod_path, heatmap_with_contours(v.lod, c.size, tg::vec3f(0, 0, 0))));
            written.push_back(lod_path);
        }

    if (captures.size() < 2)
        return written;
    auto const& a = captures[0];
    for (auto bi = 1; bi < int(captures.size()); ++bi)
        for (auto vi = 0; vi < int(a.views.size()) && vi < int(captures[bi].views.size()); ++vi)
        {
            auto const& b = captures[bi];
            auto const& va = a.views[vi];
            auto const& vb = b.views[vi];
            auto const size = a.size;
            // Three rows: the renders; each level map with its own contours; both contours over the first's map (white the first's,
            // black the second's) beside the renders' difference.
            auto figure = rgb_image::make(tg::vec2i(size[0] * 2, size[1] * 3));
            figure.blit(color_panel(va, size), 0, 0);
            figure.blit(color_panel(vb, size), size[0], 0);
            figure.blit(heatmap_with_contours(va.lod, size), 0, size[1]);
            figure.blit(heatmap_with_contours(vb.lod, size), size[0], size[1]);
            figure.blit(contour_panel(va.lod, vb.lod, size), 0, 2 * size[1]);
            figure.blit(color_difference_panel(va, vb, size), size[0], 2 * size[1]);
            auto const path = cc::format("{}-{}-{}-vs-{}.png", base, view_name(va), a.backend, b.backend);
            CC_RETURN_IF_ERROR(write_png(path, figure));
            written.push_back(path);

            // The figure's number: the mean gap over the floor, away from either end of the pyramid.
            auto sum = 0.0;
            auto count = 0;
            for (auto i = 0; i < int(va.lod.size()); ++i)
                if (va.lod[i] > 0.05f && vb.lod[i] > 0.05f && va.lod[i] < float(pyramid_mips - 1) - 0.05f
                    && vb.lod[i] < float(pyramid_mips - 1) - 0.05f)
                {
                    sum += va.lod[i] - vb.lod[i];
                    ++count;
                }
            cc::println("{}: {} minus {} = {:+.3f} LOD, mean over {} pixels", view_name(va), a.backend, b.backend,
                        count == 0 ? 0.0 : sum / count, count);
        }
    return written;
}
} // namespace lod_probe
