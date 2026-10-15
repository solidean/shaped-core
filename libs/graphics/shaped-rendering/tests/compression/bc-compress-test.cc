#include <clean-core/common/utility.hh> // cc::clamp, cc::max
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh> // cc::popcount
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/compression/bc_compress_routine.hh>
#include <sr_test_sgl_shaders.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::log


using namespace cc::primitive_defines;

// sr::bc_compress_routine, judged by the hardware's own decoder.
//
// Every comparison below loads the compressed texture and its source through one shader, so what is measured is what a
// sampler would return; a CPU decoder written beside the encoders would share whatever they got wrong.
// Each format is held only to the channels it stores: BC4 to red, BC5 to red and green, BC1 to color and one-bit alpha.
// The BC7 partition case is the one that pins its tables: block p's two subsets are four-level ramps along two different
// directions, which partition p reproduces exactly and every other partition mixes into a subset no line fits.
// So an encoder whose mask or anchor for p were wrong would find no exact fit, and the hardware's decode would show it.

namespace
{
constexpr auto source_usage = sg::texture_usage::texture | sg::texture_usage::copy_dst;
constexpr auto target_usage = sg::texture_usage::texture | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

/// The 64 two-subset partitions as their masks: bit i set where texel i, row-major in its block, is in subset 1.
constexpr u32 partition_masks[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};

struct image
{
    int width = 0;
    int height = 0;
    cc::vector<byte> rgba8;

    void set(int x, int y, u8 r, u8 g, u8 b, u8 a)
    {
        auto const at = (isize(y) * width + x) * 4;
        rgba8[at + 0] = byte(r);
        rgba8[at + 1] = byte(g);
        rgba8[at + 2] = byte(b);
        rgba8[at + 3] = byte(a);
    }
};

image blank(int width, int height)
{
    return {.width = width,
            .height = height,
            .rgba8 = cc::vector<byte>::create_filled(isize(width) * height * 4, byte(0))};
}

/// A hash to [0, 255], so the noise is the same on every run and every machine.
u8 noise(int x, int y, int c)
{
    auto h = u32(x) * 73856093u ^ u32(y) * 19349663u ^ u32(c) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return u8(h & 0xFFu);
}

/// BC7's own interpolation, which is what a ramp has to land on to be reproduced to the bit.
u8 bc7_lerp(int a, int b, int weight)
{
    return u8(((64 - weight) * a + weight * b + 32) >> 6);
}

/// Eight blocks across and eight down: block p holds partition p's subsets as two four-level ramps, one reddening and
/// one bluing, each level an interpolant BC7's two-bit indices land on exactly.
/// No other partition splits a block into two groups that each lie on a line, which an offline check over all 64
/// confirmed, so only a correct table entry for p reproduces block p.
/// The endpoints are even in every channel, so mode 3's per-endpoint p-bit can hold them exactly.
image partition_ramps()
{
    constexpr int weights[4] = {0, 21, 43, 64};
    auto out = blank(32, 32);
    for (auto p = 0; p < 64; ++p)
    {
        // Every subset holds both its endpoints, its first texel and its last, so the line it lies on is the whole ramp;
        // a three-texel subset holding only three of the four levels would not be reproducible from them.
        auto const ones = cc::popcount(partition_masks[p]);
        int const sizes[2] = {16 - ones, ones};
        int seen[2] = {0, 0};
        for (auto i = 0; i < 16; ++i)
        {
            auto const subset = int((partition_masks[p] >> i) & 1u);
            auto const j = seen[subset]++;
            auto level = 1 + (j - 1) % 2;
            if (j == 0)
                level = 0;
            else if (j == sizes[subset] - 1)
                level = 3;
            auto const w = weights[level];
            auto const x = (p % 8) * 4 + i % 4;
            auto const y = (p / 8) * 4 + i / 4;
            if (subset == 0)
                out.set(x, y, bc7_lerp(40, 200, w), 100, 60, 255);
            else
                out.set(x, y, 120, 30, bc7_lerp(40, 220, w), 255);
        }
    }
    return out;
}

/// Eight blocks across and eight down, block p holding partition p's two subsets in one color each: every channel takes
/// two values per block, which BC1's and BC4's endpoints hold as they are.
image two_colors_per_block()
{
    auto out = blank(32, 32);
    for (auto p = 0; p < 64; ++p)
        for (auto i = 0; i < 16; ++i)
        {
            auto const in_1 = ((partition_masks[p] >> i) & 1u) != 0u;
            auto const x = (p % 8) * 4 + i % 4;
            auto const y = (p / 8) * 4 + i / 4;
            if (in_1)
                out.set(x, y, u8(40 + p), 200, u8(255 - p * 3), 255);
            else
                out.set(x, y, 230, u8(20 + p * 2), 90, 255);
        }
    return out;
}

/// A smooth diagonal gradient with a little texture noise over it, opaque — what most of a material's maps look like.
image gradient(int size)
{
    auto out = blank(size, size);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const base = [&](int c) { return (x * (3 + c) + y * (5 - c)) * 255 / (size * 8); };
            auto const grain = [&](int c) { return int(noise(x, y, c)) / 16 - 8; };
            auto const channel = [&](int c) { return u8(cc::clamp(base(c) + grain(c), 0, 255)); };
            out.set(x, y, channel(0), channel(1), channel(2), 255);
        }
    return out;
}

/// `source` grown to `size` x `size` by repeating its last row and column: what an encoder's edge clamp reads.
image edge_clamped(image const& source, int size)
{
    auto out = blank(size, size);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const from = (isize(cc::min(y, source.height - 1)) * source.width + cc::min(x, source.width - 1)) * 4;
            auto const to = (isize(y) * size + x) * 4;
            for (auto c = 0; c < 4; ++c)
                out.rgba8[to + c] = source.rgba8[from + c];
        }
    return out;
}

/// A color ramp under an alpha ramp that crosses it, so no block is opaque.
image alpha_ramp(int size)
{
    auto out = blank(size, size);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            out.set(x, y, u8(x * 255 / (size - 1)), u8(y * 255 / (size - 1)), 128, u8((x + y) * 255 / (2 * size - 2)));
    return out;
}

/// A checker of single texels in two colors whose channels move in opposite directions, so the box diagonal of every
/// block is orthogonal to the one line its colors lie on.
image red_green_checker(int size)
{
    auto out = blank(size, size);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            if ((x + y) % 2 == 0)
                out.set(x, y, 255, 0, 0, 255);
            else
                out.set(x, y, 0, 255, 0, 255);
    return out;
}

/// Green, with one red texel in every block: a color the block's mean would erase.
image one_red_dot(int size)
{
    auto out = blank(size, size);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            if (x % 4 == 1 && y % 4 == 1)
                out.set(x, y, 255, 0, 0, 255);
            else
                out.set(x, y, 0, 255, 0, 255);
    return out;
}

/// The gradient, with every third texel along a diagonal cut out: what BC1's one-bit alpha exists for.
image cutout(int size)
{
    auto out = gradient(size);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            if ((x + y) % 3 == 0)
                out.rgba8[(isize(y) * size + x) * 4 + 3] = byte(0);
    return out;
}

/// What a shader loads from mip `level` of `texture`, as floats, texel by texel.
cc::shared_async<cc::vector<tg::vec4f>> decode(sg::context& ctx, sg::texture_2d const& texture, int level = 0)
{
    auto const width = cc::max(1, texture.width() >> level);
    auto const height = cc::max(1, texture.height() >> level);
    auto const& entry = sr_test::sgl_shaders::bc_decode.main_cs;
    auto const shader = entry->acquire(ctx);
    co_await cc::async_settled(shader);
    REQUIRE(shader->try_value() != nullptr);
    auto const pipeline
        = ctx.cached.acquire_compute_pipeline({.shader = *shader->try_value(), .layout = entry.acquire_layout(ctx)});
    auto const built = co_await pipeline;
    REQUIRE(built != nullptr);
    auto const layout = ctx.cached.acquire_binding_group_layout<sr_test::sgl_shaders::decode_io>();

    auto const count = isize(width) * height;
    auto cmd = ctx.create_command_list();
    auto const texels
        = ctx.transient.create_buffer<tg::vec4f>(count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout,
        sr_test::sgl_shaders::decode_io{.source = texture.as_texture_view({.mips = {.start = level, .count = 1}}),
                                        .texels = texels.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*built);
    cmd->compute.bind_group(0, *group);
    cmd->compute.set_inline_constants(sr_test::sgl_shaders::decode_constants{.width = width, .height = height}.to_block());
    cmd->compute.dispatch_threads(width, height);
    auto readback = cmd->download.data_from_buffer(texels);
    ctx.submit_command_list(cc::move(cmd));

    auto const data = co_await readback.data();
    auto out = cc::vector<tg::vec4f>();
    for (auto const& t : data)
        out.push_back(t);
    co_return out;
}

/// PSNR over the first `channels` of two decodes, in dB.
float psnr_of(cc::vector<tg::vec4f> const& want, cc::vector<tg::vec4f> const& got, int channels)
{
    auto sum = 0.0;
    auto count = isize(0);
    for (auto i = isize(0); i < want.size(); ++i)
        for (auto c = 0; c < channels; ++c)
        {
            auto const d = double((got[i][c] - want[i][c]) * 255.0f);
            sum += d * d;
            ++count;
        }
    auto const mse = sum / double(cc::max(count, isize(1)));
    return mse <= 0.0 ? 99.0f : float(10.0 * tg::log(255.0 * 255.0 / mse) / tg::log(10.0));
}

struct comparison
{
    float max_error = 0.0f;   ///< the largest difference in any compared channel, in units of 1/255
    float psnr = 0.0f;        ///< over the compared channels, in dB
    float min_alpha = 1.0f;   ///< the lowest alpha the compressed texture decodes to, compared or not
    int alpha_mismatches = 0; ///< texels whose decoded alpha is more than half a step from the source's
};

/// Compresses `source`, uploaded as `source_format`, into `target`, and compares the first `channels` of the two decodes.
/// With `opaque_only`, a texel the source holds under half alpha is compared on its alpha alone.
cc::shared_async<comparison> compress_and_compare(sg::context& ctx,
                                                  image const& source,
                                                  sg::pixel_format source_format,
                                                  sg::pixel_format target,
                                                  int channels,
                                                  bool opaque_only = false,
                                                  sr::bc_compress_options const& opts = {})
{
    auto const original = ctx.persistent.create_texture_2d(
        {.format = source_format, .width = source.width, .height = source.height, .usage = source_usage});
    auto const compressed = ctx.persistent.create_texture_2d(
        {.format = target, .width = source.width, .height = source.height, .usage = target_usage});

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(original.raw(), cc::span<byte const>(source.rgba8));
    auto const outcome = sr::bc_compress_routine::execute(*cmd, original, compressed, 0, opts);
    ctx.submit_command_list(cc::move(cmd));
    REQUIRE(outcome == sg::routine_outcome::executed);

    auto const want = co_await decode(ctx, original);
    auto const got = co_await decode(ctx, compressed);
    REQUIRE(want.size() == got.size());

    auto out = comparison();
    auto sum = 0.0;
    auto count = isize(0);
    for (auto i = isize(0); i < want.size(); ++i)
    {
        out.min_alpha = cc::min(out.min_alpha, got[i][3]);
        auto const alpha_miss = got[i][3] - want[i][3];
        if (alpha_miss > 0.5f / 255.0f || alpha_miss < -0.5f / 255.0f)
            ++out.alpha_mismatches;
        auto const cut = opaque_only && want[i][3] < 0.5f;
        for (auto c = 0; c < channels; ++c)
        {
            if (cut && c < 3)
                continue;
            auto const d = (got[i][c] - want[i][c]) * 255.0f;
            out.max_error = cc::max(out.max_error, d < 0.0f ? -d : d);
            sum += double(d) * double(d);
            ++count;
        }
    }
    auto const mse = sum / double(cc::max(count, isize(1)));
    out.psnr = mse <= 0.0 ? 99.0f : float(10.0 * tg::log(255.0 * 255.0 / mse) / tg::log(10.0));
    co_return out;
}

/// rgba32 floats over a `size` x `size` image: red ramps -1 to 1 across, green -1 to 1 down — a normal map's x and y.
cc::vector<float> signed_ramp(int size)
{
    auto out = cc::vector<float>::create_filled(isize(size) * size * 4, 0.0f);
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const at = (isize(y) * size + x) * 4;
            out[at + 0] = -1.0f + 2.0f * float(x) / float(size - 1);
            out[at + 1] = -1.0f + 2.0f * float(y) / float(size - 1);
            out[at + 3] = 1.0f;
        }
    return out;
}

/// Compresses `values`, an rgba32-float image `size` texels square, into `target`, and returns what a shader loads back.
cc::shared_async<cc::vector<tg::vec4f>> compress_floats(sg::context& ctx,
                                                        cc::vector<float> const& values,
                                                        int size,
                                                        sg::pixel_format target,
                                                        sr::bc_compress_options const& opts)
{
    auto const original = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float, .width = size, .height = size, .usage = source_usage});
    auto const compressed
        = ctx.persistent.create_texture_2d({.format = target, .width = size, .height = size, .usage = target_usage});

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(original.raw(), cc::span<float const>(values).as_bytes());
    auto const outcome = sr::bc_compress_routine::execute(*cmd, original, compressed, 0, opts);
    ctx.submit_command_list(cc::move(cmd));
    REQUIRE(outcome == sg::routine_outcome::executed);
    co_return co_await decode(ctx, compressed);
}

/// Brings up the routine's instance for each of `families`, so a first execute does not decline.
cc::shared_async<cc::unit> prewarm(sg::context& ctx, std::initializer_list<sg::pixel_format> families)
{
    // WORKAROUND, the one the mipmap tests carry: a routine executes only once a tick has brought it up.
    for (auto const f : families)
        sr::bc_compress_routine::prewarm(ctx, f);
    (void)co_await ctx.routines.idle_completion();
    for (auto const f : families)
        REQUIRE(sr::bc_compress_routine::try_acquire(ctx, f).is_ready());
    co_return;
}
} // namespace

TEST("sr - target_for keeps a signed target signed, and takes an sRGB source's encoding")
{
    // A linear source keeps the target as asked: only an sRGB encoding is ever taken off, never the sign.
    CHECK(sr::bc_compress_routine::target_for(sg::pixel_format::rgba32_float, sg::pixel_format::bc5_rg_snorm)
          == sg::pixel_format::bc5_rg_snorm);
    CHECK(sr::bc_compress_routine::target_for(sg::pixel_format::rgba32_float, sg::pixel_format::bc4_r_snorm)
          == sg::pixel_format::bc4_r_snorm);
    CHECK(sr::bc_compress_routine::target_for(sg::pixel_format::rgba8_unorm, sg::pixel_format::bc7_rgba_unorm_srgb)
          == sg::pixel_format::bc7_rgba_unorm);
    CHECK(sr::bc_compress_routine::target_for(sg::pixel_format::rgba8_unorm_srgb, sg::pixel_format::bc7_rgba_unorm)
          == sg::pixel_format::bc7_rgba_unorm_srgb);
}

ASYNC_INVOCABLE_TEST("sr - BC1 and BC7 compress any mip level, a partial block and a single texel included",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    auto constexpr source_format = sg::pixel_format::rgba8_unorm;
    if (!sr::bc_compress_routine::can_compress(ctx, source_format, sg::pixel_format::bc7_rgba_unorm))
        SKIP("this device has no BC textures");
    co_await prewarm(ctx, {sg::pixel_format::bc1_rgba_unorm, sg::pixel_format::bc7_rgba_unorm});

    // 64 x 64 with every mip, each its own gradient, so a level read from the wrong place shows as a wrong image.
    constexpr int size = 64;
    constexpr int mips = 7;
    auto const original = ctx.persistent.create_texture_2d(
        {.format = source_format, .width = size, .height = size, .mip_levels = mips, .usage = source_usage});
    auto cmd = ctx.create_command_list();
    for (auto l = 0; l < mips; ++l)
        cmd->upload.bytes_to_texture(original.raw(), cc::span<byte const>(gradient(size >> l).rgba8), {.mip_level = l});
    ctx.submit_command_list(cc::move(cmd));

    struct target_case
    {
        sg::pixel_format format = sg::pixel_format::bc1_rgba_unorm;
        int channels = 3;
        float min_psnr = 32.0f;
    };
    // Levels 1, 4 and 6: 32 x 32, one 4 x 4 block, and a single texel, which is a block the edge clamp fills.
    // Each must decode exactly as the same image does compressed as level 0 of a texture of its own, which pins the
    // level's extent, its view and the edge clamp.
    // Only the 32 x 32 level is also held to a quality floor: at 4 x 4, `gradient` puts its whole ramp and its noise in
    // one block, which no BC1 or BC7 block holds well whatever the level.
    for (auto const t : {target_case{.format = sg::pixel_format::bc1_rgba_unorm, .channels = 3, .min_psnr = 32.0f},
                         target_case{.format = sg::pixel_format::bc7_rgba_unorm, .channels = 4, .min_psnr = 38.0f}})
    {
        auto const compressed = ctx.persistent.create_texture_2d(
            {.format = t.format, .width = size, .height = size, .mip_levels = mips, .usage = target_usage});
        for (auto const level : {1, 4, 6})
        {
            auto const extent = size >> level;

            // A device without unaligned BC (metal) refuses a texture smaller than a block, though it has the mip.
            // There the reference is the level edge-clamped to whole blocks, which the encoder must read alike.
            auto const alone_extent
                = ctx.supports(sg::feature::unaligned_block_compression) ? extent : (extent + 3) / 4 * 4;
            auto const alone_source = ctx.persistent.create_texture_2d(
                {.format = source_format, .width = alone_extent, .height = alone_extent, .usage = source_usage});
            auto const alone = ctx.persistent.create_texture_2d(
                {.format = t.format, .width = alone_extent, .height = alone_extent, .usage = target_usage});

            auto encode = ctx.create_command_list();
            encode->upload.bytes_to_texture(alone_source.raw(),
                                            cc::span<byte const>(edge_clamped(gradient(extent), alone_extent).rgba8));
            auto const outcome = sr::bc_compress_routine::execute(*encode, original, compressed, level);
            auto const alone_outcome = sr::bc_compress_routine::execute(*encode, alone_source, alone, 0);
            ctx.submit_command_list(cc::move(encode));
            REQUIRE(outcome == sg::routine_outcome::executed);
            REQUIRE(alone_outcome == sg::routine_outcome::executed);

            auto const want = co_await decode(ctx, original, level);
            auto const got = co_await decode(ctx, compressed, level);
            auto const reference = co_await decode(ctx, alone);
            REQUIRE(want.size() == got.size());
            REQUIRE(reference.size() == isize(alone_extent) * alone_extent);

            auto differing = 0;
            for (auto y = 0; y < extent; ++y)
                for (auto x = 0; x < extent; ++x)
                    differing += got[isize(y) * extent + x] == reference[isize(y) * alone_extent + x] ? 0 : 1;
            CHECK(differing == 0).dump("level", level).dump("differing texels", differing).dump("channels", t.channels);

            auto const psnr = psnr_of(want, got, t.channels);
            if (level == 1)
                CHECK(psnr >= t.min_psnr).dump("level", level).dump("psnr", psnr).dump("channels", t.channels);
            CC_LOG_INFO("mip {} of {} channels: {} dB, {} texels differing from level 0", level, t.channels, psnr,
                        differing);
        }
    }
}

ASYNC_INVOCABLE_TEST("sr - BC7 compression reproduces every two-subset partition and holds a gradient",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    auto constexpr bc7 = sg::pixel_format::bc7_rgba_unorm;
    if (!sr::bc_compress_routine::can_compress(ctx, sg::pixel_format::rgba8_unorm, bc7))
        SKIP("this device has no BC textures");
    co_await prewarm(ctx, {bc7});

    // Two ramps per block that only partition p fits: a mode 3 endpoint holds 8 bits with its p-bit, so at most that
    // rounding may remain, and a wrong mask or anchor would leave far more.
    auto const exact = co_await compress_and_compare(ctx, partition_ramps(), sg::pixel_format::rgba8_unorm, bc7, 4);
    CHECK(exact.max_error <= 2.0f).dump("max error", exact.max_error).dump("psnr", exact.psnr);

    auto const smooth = co_await compress_and_compare(ctx, gradient(64), sg::pixel_format::rgba8_unorm, bc7, 4);
    CHECK(smooth.psnr >= 38.0f).dump("psnr", smooth.psnr).dump("max error", smooth.max_error);

    // An opaque source decodes opaque at every texel, exactly: mode 6 holds its p-bits at 1 for an opaque block.
    CHECK(smooth.min_alpha == 1.0f).dump("min alpha", smooth.min_alpha);
    CHECK(exact.min_alpha == 1.0f).dump("min alpha", exact.min_alpha);

    // Two colors whose channels move in opposite directions, which no starting guess built from the box may lose.
    auto const checker = co_await compress_and_compare(ctx, red_green_checker(16), sg::pixel_format::rgba8_unorm, bc7, 4);
    CHECK(checker.max_error <= 2.0f).dump("max error", checker.max_error);
    auto const dot = co_await compress_and_compare(ctx, one_red_dot(16), sg::pixel_format::rgba8_unorm, bc7, 4);
    CHECK(dot.max_error <= 2.0f).dump("max error", dot.max_error);

    // Color and alpha each ramp along their own axis, so no single line fits a block; mode 7's two subsets fit it closely.
    auto const alpha = co_await compress_and_compare(ctx, alpha_ramp(32), sg::pixel_format::rgba8_unorm, bc7, 4);
    CHECK(alpha.psnr >= 38.0f).dump("psnr", alpha.psnr).dump("max error", alpha.max_error);

    // An sRGB source is encoded as the bytes it stores, so the two decode to the same linear colors.
    auto const srgb = co_await compress_and_compare(ctx, gradient(64), sg::pixel_format::rgba8_unorm_srgb,
                                                    sg::pixel_format::bc7_rgba_unorm_srgb, 4);
    CHECK(srgb.psnr >= 38.0f).dump("psnr", srgb.psnr).dump("max error", srgb.max_error);

    CC_LOG_INFO("bc7: partitions max error {}, gradient {} dB, alpha ramp {} dB, sRGB gradient {} dB", exact.max_error,
                smooth.psnr, alpha.psnr, srgb.psnr);
}

ASYNC_INVOCABLE_TEST("sr - BC1 and BC3 compression hold color, and BC1 cuts out where alpha is under half",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    auto constexpr bc1 = sg::pixel_format::bc1_rgba_unorm;
    auto constexpr bc3 = sg::pixel_format::bc3_unorm;
    if (!sr::bc_compress_routine::can_compress(ctx, sg::pixel_format::rgba8_unorm, bc1))
        SKIP("this device has no BC textures");
    co_await prewarm(ctx, {bc1, bc3});

    // Two colors per block, so only the 5:6:5 rounding of the endpoints remains: under 5 for a 5-bit channel, whose
    // levels lie 8 or 9 apart.
    auto const exact = co_await compress_and_compare(ctx, two_colors_per_block(), sg::pixel_format::rgba8_unorm, bc1, 3);
    CHECK(exact.max_error <= 5.0f).dump("max error", exact.max_error).dump("psnr", exact.psnr);

    auto const smooth = co_await compress_and_compare(ctx, gradient(64), sg::pixel_format::rgba8_unorm, bc1, 3);
    CHECK(smooth.psnr >= 32.0f).dump("psnr", smooth.psnr).dump("max error", smooth.max_error);

    // Two colors whose channels move in opposite directions, which no starting guess built from the box may lose.
    auto const checker = co_await compress_and_compare(ctx, red_green_checker(16), sg::pixel_format::rgba8_unorm, bc1, 3);
    CHECK(checker.max_error <= 5.0f).dump("max error", checker.max_error);
    auto const dot = co_await compress_and_compare(ctx, one_red_dot(16), sg::pixel_format::rgba8_unorm, bc1, 3);
    CHECK(dot.max_error <= 5.0f).dump("max error", dot.max_error);

    // Asked to, BC1 cuts out the texels under half alpha: every texel decodes at exactly the source's 0 or 1 alpha, and
    // the color of the ones kept holds to 30 dB.
    auto const cut = co_await compress_and_compare(ctx, cutout(32), sg::pixel_format::rgba8_unorm, bc1, 4, true,
                                                   {.bc1_alpha = sr::bc1_alpha::cut_out});
    CHECK(cut.alpha_mismatches == 0).dump("alpha mismatches", cut.alpha_mismatches);
    CHECK(cut.psnr >= 30.0f).dump("psnr", cut.psnr).dump("max error", cut.max_error);

    // By default the same source is encoded opaque: alpha is ignored, and nothing is cut out.
    auto const kept = co_await compress_and_compare(ctx, cutout(32), sg::pixel_format::rgba8_unorm, bc1, 3);
    CHECK(kept.min_alpha == 1.0f).dump("min alpha", kept.min_alpha);
    CHECK(kept.psnr >= 32.0f).dump("psnr", kept.psnr);

    // BC3's alpha is a BC4 block of its own, so a smooth alpha survives where BC1's one bit could not hold it.
    auto const alpha = co_await compress_and_compare(ctx, alpha_ramp(32), sg::pixel_format::rgba8_unorm, bc3, 4);
    CHECK(alpha.psnr >= 32.0f).dump("psnr", alpha.psnr).dump("max error", alpha.max_error);

    auto const srgb = co_await compress_and_compare(ctx, gradient(64), sg::pixel_format::rgba8_unorm_srgb,
                                                    sg::pixel_format::bc1_rgba_unorm_srgb, 3);
    CHECK(srgb.psnr >= 32.0f).dump("psnr", srgb.psnr).dump("max error", srgb.max_error);

    CC_LOG_INFO("bc1: two colors max error {}, gradient {} dB, cut out {} dB, sRGB gradient {} dB; bc3: alpha ramp {} "
                "dB",
                exact.max_error, smooth.psnr, cut.psnr, srgb.psnr, alpha.psnr);
}

ASYNC_INVOCABLE_TEST("sr - BC4 and BC5 compression hold one and two channels",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    auto constexpr bc4 = sg::pixel_format::bc4_r_unorm;
    auto constexpr bc5 = sg::pixel_format::bc5_rg_unorm;
    if (!sr::bc_compress_routine::can_compress(ctx, sg::pixel_format::rgba8_unorm, bc4))
        SKIP("this device has no BC textures");
    co_await prewarm(ctx, {bc4, bc5});

    // Eight levels per block over its own range: a smooth channel loses a fraction of a step.
    auto const one = co_await compress_and_compare(ctx, gradient(64), sg::pixel_format::rgba8_unorm, bc4, 1);
    CHECK(one.psnr >= 40.0f).dump("psnr", one.psnr).dump("max error", one.max_error);

    auto const two = co_await compress_and_compare(ctx, gradient(64), sg::pixel_format::rgba8_unorm, bc5, 2);
    CHECK(two.psnr >= 40.0f).dump("psnr", two.psnr).dump("max error", two.max_error);

    // A channel that takes two values across a block keeps both exactly: they are the block's endpoints.
    auto const flat = co_await compress_and_compare(ctx, two_colors_per_block(), sg::pixel_format::rgba8_unorm, bc4, 1);
    CHECK(flat.max_error <= 1.0f).dump("max error", flat.max_error);

    // An sRGB source cannot target either: they have no sRGB form, and a linear channel is what they are for.
    CHECK(!sr::bc_compress_routine::can_compress(ctx, sg::pixel_format::rgba8_unorm_srgb, bc4));

    CC_LOG_INFO("bc4: gradient {} dB, two-valued blocks max error {}; bc5: gradient {} dB", one.psnr, flat.max_error,
                two.psnr);
}

ASYNC_INVOCABLE_TEST("sr - BC4 and BC5 read a signed source as asked, into their unsigned and their signed forms",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    auto constexpr source = sg::pixel_format::rgba32_float;
    if (!sr::bc_compress_routine::can_compress(ctx, source, sg::pixel_format::bc5_rg_snorm))
        SKIP("this device has no BC textures");

    // Block compression stores normalized or float data only: integer and depth sources have no target.
    CHECK(!sr::bc_compress_routine::can_compress(ctx, sg::pixel_format::r8_uint, sg::pixel_format::bc4_r_unorm));
    CHECK(!sr::bc_compress_routine::can_compress(ctx, sg::pixel_format::rgba16_sint, sg::pixel_format::bc7_rgba_unorm));
    CHECK(!sr::bc_compress_routine::can_compress(ctx, sg::pixel_format::depth32_float, sg::pixel_format::bc4_r_unorm));
    CHECK(sr::bc_compress_routine::can_compress(ctx, source, sg::pixel_format::bc4_r_snorm));

    // The signed forms share their unsigned sibling's instance.
    co_await prewarm(ctx, {sg::pixel_format::bc4_r_unorm, sg::pixel_format::bc5_rg_unorm});

    constexpr int size = 32;
    auto const values = signed_ramp(size);
    auto const signed_range = sr::bc_source_range::minus_one_to_one;
    auto const as_unorm
        = co_await compress_floats(ctx, values, size, sg::pixel_format::bc5_rg_unorm, {.bc5_range = signed_range});
    auto const as_snorm
        = co_await compress_floats(ctx, values, size, sg::pixel_format::bc5_rg_snorm, {.bc5_range = signed_range});
    auto const clamped = co_await compress_floats(ctx, values, size, sg::pixel_format::bc5_rg_unorm, {});
    auto const bc4_snorm
        = co_await compress_floats(ctx, values, size, sg::pixel_format::bc4_r_snorm, {.bc4_range = signed_range});

    // The ramp crosses about 25 of 255 levels in a block's four texels, so BC4's eight levels lie about 3.5 apart.
    // Half that, plus the source's own rounding to a level, is what a texel may miss by.
    auto unorm_error = 0.0f;
    auto snorm_error = 0.0f;
    auto clamp_error = 0.0f;
    auto bc4_error = 0.0f;
    for (auto i = isize(0); i < isize(size) * size; ++i)
    {
        for (auto c = 0; c < 2; ++c)
        {
            auto const v = values[i * 4 + c];
            unorm_error = cc::max(unorm_error, tg::abs(as_unorm[i][c] - (v * 0.5f + 0.5f)) * 255.0f);
            snorm_error = cc::max(snorm_error, tg::abs(as_snorm[i][c] - v) * 127.0f);
            clamp_error = cc::max(clamp_error, tg::abs(clamped[i][c] - cc::max(v, 0.0f)) * 255.0f);
        }
        bc4_error = cc::max(bc4_error, tg::abs(bc4_snorm[i][0] - values[i * 4]) * 127.0f);
    }
    CHECK(unorm_error <= 3.0f).dump("steps", unorm_error); // -1..1 stored as n * 0.5 + 0.5
    CHECK(snorm_error <= 3.0f).dump("steps", snorm_error); // -1..1 stored as is, signed
    CHECK(clamp_error <= 3.0f).dump("steps", clamp_error); // the default reads 0..1: the negative half clamps to 0
    CHECK(bc4_error <= 3.0f).dump("steps", bc4_error);     // one channel, signed
    CHECK(as_snorm[0][0] < -0.95f).dump("decoded", as_snorm[0][0]); // the corner at -1 stays negative

    CC_LOG_INFO("bc5 signed source: unsigned form {} steps, signed form {}, clamped {}; bc4 signed form {} steps",
                unorm_error, snorm_error, clamp_error, bc4_error);
}
