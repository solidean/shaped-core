#include "../shaders/shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_mixed_dxil.hh>
#include <sg_test_mixed_spirv.hh>
#include <sg_test_mixed_text.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

using float4 = tg::vec<4, f32>;
using uint4 = tg::vec<4, u32>;

// sgl_mixed/pack_routes.sgl through DXC and through the context's native route in one context, bit for bit:
// every half and every byte unpacked, every half packed back, and the values a pack has to round, ties among them.
// Each route is held to the CPU as well: exactly, but for the roundings the spec leaves a target, which it names,
// so two routes cannot agree on an answer the spec allows no target.

namespace
{
f32 float_of(u32 bits)
{
    return cc::bit_cast<f32>(bits);
}

u32 bits_of(f32 x)
{
    return cc::bit_cast<u32>(x);
}

bool is_nan(f32 x)
{
    return x != x;
}

/// The float half `h` stands for, exactly, a NaN's payload kept.
f32 float_of_half(u32 h)
{
    auto const sign = (h & 0x8000u) << 16;
    auto const exponent = (h >> 10) & 0x1fu;
    auto const mantissa = h & 0x3ffu;
    if (exponent == 0)
        return float_of(sign | bits_of(f32(mantissa) * 5.9604644775390625e-08f));
    if (exponent == 31)
        return float_of(sign | 0x7f800000u | (mantissa << 13));
    return float_of(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

/// The float midway between finite half `h` and the next one up in magnitude, 65520 past the largest.
f32 midpoint_of_half(u32 h)
{
    if ((h & 0x7c00u) == 0)
        return float_of(((h & 0x8000u) << 16) | bits_of(f32(2 * (h & 0x3ffu) + 1) * 2.98023223876953125e-08f));
    return float_of(bits_of(float_of_half(h)) + 0x1000u);
}

/// `x` as a half rounded to nearest with ties to even, or toward zero; a NaN as the quiet one.
u32 half_of(f32 x, bool toward_zero)
{
    if (is_nan(x))
        return 0x7e00u;
    auto const sign = (bits_of(x) >> 16) & 0x8000u;
    auto const a = f64(tg::abs(x));
    if (a == f64(float_of(0x7f800000u)))
        return sign | 0x7c00u;
    // the largest finite half not above `a`: a half's bits order its magnitudes
    auto low = 0u;
    auto high = 0x7c00u;
    while (high - low > 1)
    {
        auto const mid = (low + high) / 2;
        (f64(float_of_half(mid)) <= a ? low : high) = mid;
    }
    if (a == f64(float_of_half(low)) || toward_zero)
        return sign | low;
    auto const below = a - f64(float_of_half(low));
    auto const above = (low == 0x7bffu ? 65536.0 : f64(float_of_half(low + 1))) - a;
    return sign | (below < above ? low : above < below ? low + 1 : (low & 1u) == 0 ? low : low + 1);
}

u32 halves_of(f32 x, f32 y, bool toward_zero)
{
    return half_of(x, toward_zero) | (half_of(y, toward_zero) << 16);
}

/// Both halves of a word, a NaN as the quiet one, since no target promises a NaN's payload.
u32 canonical_halves(u32 word)
{
    auto const one = [](u32 h) { return (h & 0x7c00u) == 0x7c00u && (h & 0x3ffu) != 0 ? 0x7e00u : h; };
    return one(word & 0xffffu) | (one(word >> 16) << 16);
}

/// How a pack's byte may round its lane times 255 or 127 (EVAL-88): `floor(scaled + 0.5)` with the product and the
/// sum each rounded to a float, or fused into one multiply-add, which EVAL-6 allows at `.fast`;
/// or the product rounded to a float and then to the nearest byte, a tie to the even one, as GLSL.std.450's packs do.
enum class byte_rounding
{
    unfused,
    fused,
    nearest_even,
};

i32 byte_of(f32 unit, f32 scale, byte_rounding r)
{
    auto const scaled = unit * scale;
    switch (r)
    {
    case byte_rounding::unfused:
        return i32(tg::floor(scaled + 0.5f));
    case byte_rounding::fused:
        // a float times 255 or 127 is exact in a double
        return i32(tg::floor(f64(unit) * f64(scale) + 0.5));
    case byte_rounding::nearest_even:
    {
        auto const below = tg::floor(scaled);
        auto const rest = scaled - below;
        return i32(below) + (rest > 0.5f || (rest == 0.5f && i32(below) % 2 != 0) ? 1 : 0);
    }
    }
    return 0;
}

/// The unit a lane is clamped to before it is scaled.
f32 unit_of(f32 x, bool is_snorm)
{
    return cc::clamp(x, is_snorm ? -1.0f : 0.0f, 1.0f);
}

/// `v` packed as unorm or snorm bytes, every lane rounded by `r`.
u32 bytes_of(float4 v, bool is_snorm, byte_rounding r)
{
    auto packed = 0u;
    for (auto l = 0; l < 4; ++l)
        packed |= (u32(byte_of(unit_of(v[l], is_snorm), is_snorm ? 127.0f : 255.0f, r)) & 0xffu) << (8 * l);
    return packed;
}

/// Whether each byte of `word` is a rounding of its lane EVAL-88 allows, or the nearest one where `nearest_is_allowed`.
bool are_bytes_of(u32 word, float4 v, bool is_snorm, bool nearest_is_allowed)
{
    for (auto l = 0; l < 4; ++l)
    {
        auto const byte = (word >> (8 * l)) & 0xffu;
        auto const unit = unit_of(v[l], is_snorm);
        auto const scale = is_snorm ? 127.0f : 255.0f;
        auto const is = [&](byte_rounding r) { return byte == (u32(byte_of(unit, scale, r)) & 0xffu); };
        if (!is(byte_rounding::unfused) && !is(byte_rounding::fused)
            && !(nearest_is_allowed && is(byte_rounding::nearest_even)))
            return false;
    }
    return true;
}

/// Whether the roundings `byte_rounding` names disagree on a byte of `v`.
bool is_byte_tie(float4 v, bool is_snorm)
{
    auto const unfused = bytes_of(v, is_snorm, byte_rounding::unfused);
    return unfused != bytes_of(v, is_snorm, byte_rounding::fused)
        || unfused != bytes_of(v, is_snorm, byte_rounding::nearest_even);
}

/// A byte unpacked: `byte / scale`, which `.fast` may compute as `byte * (1 / scale)`, as DXC does.
bool is_unpacked(f32 gpu, i32 byte, f32 scale, f32 floor)
{
    auto const quotient = cc::max(f32(byte) / scale, floor);
    auto const product = cc::max(f32(byte) * (1.0f / scale), floor);
    return bits_of(gpu) == bits_of(quotient) || bits_of(gpu) == bits_of(product);
}

/// The float whose product with `scale` rounds to exactly `target`, nearest to `target / scale`; that seed if none is
/// within eight steps.
f32 tie_near(f32 target, f32 scale)
{
    auto const seed = bits_of(target / scale);
    for (auto step = 0u; step <= 8; ++step)
        for (auto const bits : {seed + step, seed - step})
            if (float_of(bits) * scale == target)
                return float_of(bits);
    return float_of(seed);
}

f32 next_up(f32 x)
{
    return float_of(bits_of(x) + 1);
}

f32 next_down(f32 x)
{
    return float_of(bits_of(x) - 1);
}

constexpr u32 k_every_half = 0x10000u;

/// Value h for every half h: its float, the float midway to the next half up, and the floats a step either side of
/// that midpoint; an infinity or a NaN four times.
/// Then each byte's tie, the float whose product with 255 or 127 rounds to the byte and a half, with its neighbors;
/// then values past every edge.
cc::vector<float4> pack_inputs()
{
    auto values = cc::vector<float4>();
    for (auto h = 0u; h < k_every_half; ++h)
    {
        auto const x = float_of_half(h);
        auto const mid = (h & 0x7c00u) == 0x7c00u ? x : midpoint_of_half(h);
        values.push_back((h & 0x7c00u) == 0x7c00u ? float4(x, x, x, x) : float4(x, mid, next_down(mid), next_up(mid)));
    }
    for (auto k = 0; k < 256; ++k)
    {
        auto const u = tie_near(f32(k) + 0.5f, 255.0f);
        values.push_back(float4(u, next_down(u), next_up(u), f32(k) / 255.0f));
        auto const s = tie_near(f32(k - 128) + 0.5f, 127.0f);
        values.push_back(float4(s, next_down(s), next_up(s), f32(k - 128) / 127.0f));
    }
    auto const infinity = float_of(0x7f800000u);
    values.push_back(float4(1.0e-40f, -1.0e-40f, 1.0e6f, -1.0e6f));
    values.push_back(float4(infinity, -infinity, 65519.0f, 65521.0f));
    values.push_back(float4(-0.0f, 2.0f, -2.0f, 0.99999994f));
    values.push_back(float4(5.9604644775390625e-08f, 2.98023223876953125e-08f, 2.9802326e-08f, 1.0e-30f));
    while (values.size() % 64 != 0)
        values.push_back(float4(0.25f, -0.25f, 0.75f, -0.75f));
    return values;
}

/// Word i for every 16 bits i: i in the low half and its complement in the high one, so every half and every byte is
/// read in each place; then hashed bits.
cc::vector<u32> unpack_inputs(isize count)
{
    auto words = cc::vector<u32>();
    for (auto i = 0u; i < u32(count); ++i)
        words.push_back(i < k_every_half ? i | ((i ^ 0xffffu) << 16) : i * 0x9e37'79b9u);
    return words;
}

struct outputs
{
    cc::vector<uint4> packed;
    cc::vector<float4> unpacked;
};

template <class Group, class Shader>
cc::shared_async<outputs> run(sg::context& ctx,
                              Shader const& shader,
                              cc::vector<float4> const& values,
                              cc::vector<u32> const& words)
{
    auto const pipeline = co_await shader.acquire_pipeline(ctx);
    auto const layout = ctx.cached.acquire_binding_group_layout<Group>();
    auto const value_buffer = ctx.persistent.create_buffer_from_data(values, sg::buffer_usage::readonly_buffer);
    auto const word_buffer = ctx.persistent.create_buffer_from_data(words, sg::buffer_usage::readonly_buffer);
    auto const packed
        = ctx.persistent.create_buffer_from_data(cc::vector<uint4>::create_defaulted(values.size()),
                                                 sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const unpacked
        = ctx.persistent.create_buffer_from_data(cc::vector<float4>::create_defaulted(3 * values.size()),
                                                 sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto cmd = ctx.create_command_list();
    auto const group = ctx.transient.create_binding_group(*cmd, layout,
                                                          Group{.values = value_buffer.as_readonly_buffer(),
                                                                .words = word_buffer.as_readonly_buffer(),
                                                                .packed = packed.as_readwrite_buffer(),
                                                                .unpacked = unpacked.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(i32(values.size()));
    auto const packed_future = cmd->download.data_from_buffer(packed);
    auto const unpacked_future = cmd->download.data_from_buffer(unpacked);
    ctx.submit_command_list(cc::move(cmd));
    auto const packed_data = co_await packed_future.data();
    auto const unpacked_data = co_await unpacked_future.data();
    co_return outputs{.packed = cc::vector<uint4>::create_copy_of(cc::span<uint4 const>(packed_data)),
                      .unpacked = cc::vector<float4>::create_copy_of(cc::span<float4 const>(unpacked_data))};
}

/// What one route computed, held to the CPU: the unpacks exactly, the bytes by EVAL-88, and every half packed back
/// exactly; a value between two halves to the nearest, or toward zero where `.toward_zero` is counted.
struct held
{
    /// TEMPORARY(bug-sgl-spirv-pack-bytes-round-ties-to-even): a byte taken to the nearest, where that is allowed.
    bool nearest_bytes = false;
    isize toward_zero = 0;
    isize rounded = 0;
};

void hold_to_cpu(outputs const& o, cc::span<float4 const> values, cc::span<u32 const> words, cc::string_view route, held& h)
{
    auto failures = 0;
    auto const report = [&](bool ok) { return ok || failures++ >= 8; };
    for (auto i = isize(0); i < values.size(); ++i)
    {
        auto const v = values[i];
        auto const has_nan = is_nan(v[0]) || is_nan(v[1]) || is_nan(v[2]) || is_nan(v[3]);
        // EVAL-88 gives no NaN a byte that targets agree on
        if (!has_nan)
        {
            auto const unorm = o.packed[i][0];
            auto const snorm = o.packed[i][1];
            CHECK(report(are_bytes_of(unorm, v, false, h.nearest_bytes)))
                .dump("route", route)
                .dump("x", v)
                .dump("gpu", unorm)
                .dump("cpu", bytes_of(v, false, byte_rounding::unfused));
            CHECK(report(are_bytes_of(snorm, v, true, h.nearest_bytes)))
                .dump("route", route)
                .dump("x", v)
                .dump("gpu", snorm)
                .dump("cpu", bytes_of(v, true, byte_rounding::unfused));
        }
        for (auto const pair : {0, 1})
        {
            auto const x = v[2 * pair];
            auto const y = v[2 * pair + 1];
            auto const gpu = canonical_halves(o.packed[i][2 + pair]);
            auto const nearest = halves_of(x, y, false);
            auto const truncated = halves_of(x, y, true);
            auto const is_exact = nearest == truncated;
            h.rounded += is_exact ? 0 : 1;
            h.toward_zero += !is_exact && gpu == truncated ? 1 : 0;
            CHECK(report(gpu == nearest || gpu == truncated))
                .dump("route", route)
                .dump("x", x)
                .dump("y", y)
                .dump("gpu", gpu)
                .dump("nearest", nearest);
        }
    }

    for (auto i = isize(0); i < words.size(); ++i)
    {
        auto const w = words[i];
        auto const unorm = o.unpacked[3 * i];
        auto const snorm = o.unpacked[3 * i + 1];
        auto const half = o.unpacked[3 * i + 2];
        for (auto l = 0; l < 4; ++l)
        {
            auto const byte = i32((w >> (8 * l)) & 0xffu);
            CHECK(report(is_unpacked(unorm[l], byte, 255.0f, 0.0f)))
                .dump("route", route)
                .dump("word", w)
                .dump("lane", l)
                .dump("gpu", unorm[l]);
            CHECK(report(is_unpacked(snorm[l], byte > 127 ? byte - 256 : byte, 127.0f, -1.0f)))
                .dump("route", route)
                .dump("word", w)
                .dump("lane", l)
                .dump("gpu", snorm[l]);
        }
        for (auto l = 0; l < 2; ++l)
        {
            auto const expected = float_of_half((w >> (16 * l)) & 0xffffu);
            CHECK(report(bits_of(half[l]) == bits_of(expected) || (is_nan(half[l]) && is_nan(expected))))
                .dump("route", route)
                .dump("word", w)
                .dump("lane", l)
                .dump("gpu", half[l])
                .dump("cpu", expected);
        }
    }
    // a target's conversion rounds every inexact pair the same way, to nearest or toward zero
    CHECK((h.toward_zero == 0 || h.toward_zero == h.rounded)) // TEMPORARY(bug-sgl-pack-half-rounds-toward-zero)
        .dump("route", route)
        .dump("toward zero", h.toward_zero)
        .dump("inexact", h.rounded);
    CHECK(failures == 0).dump("route", route);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - every half and byte packs and unpacks alike through DXC and through SGL's binary",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    if (!takes_dxil && !ctx->accepts_shader_format(sg::shader_format::spirv))
        SKIP("this context takes no native route");
    if (ctx->prefers_native_dxil() || ctx->prefers_native_spirv())
        SKIP("this context takes SGL's binary for every package, so there is no DXC route to compare with");

    auto const values = pack_inputs();
    auto const words = unpack_inputs(values.size());

    namespace text = sg::test::mixed_text::pack_routes;
    auto const through_dxc = co_await run<text::pack_routes>(*ctx, text::pack_steps, values, words);
    auto through_native = outputs();
    if (takes_dxil)
    {
        namespace native = sg::test::mixed_dxil::pack_routes;
        auto const shader = co_await native::pack_steps->acquire(*ctx);
        CHECK(shader.compiler.name == "sgl");
        through_native = co_await run<native::pack_routes>(*ctx, native::pack_steps, values, words);
    }
    else
    {
        namespace native = sg::test::mixed_spirv::pack_routes;
        auto const shader = co_await native::pack_steps->acquire(*ctx);
        CHECK(shader.compiler.name == "sgl");
        through_native = co_await run<native::pack_routes>(*ctx, native::pack_steps, values, words);
    }
    REQUIRE(through_dxc.packed.size() == values.size());
    REQUIRE(through_native.packed.size() == values.size());
    REQUIRE(through_dxc.unpacked.size() == 3 * values.size());
    REQUIRE(through_native.unpacked.size() == 3 * values.size());

    // TEMPORARY(bug-sgl-spirv-pack-bytes-round-ties-to-even): GLSL.std.450's packs take a byte to the nearest
    auto native_held = held{.nearest_bytes = !takes_dxil};
    auto dxc_held = held();
    hold_to_cpu(through_native, values, words, "native", native_held);
    hold_to_cpu(through_dxc, values, words, "dxc", dxc_held);
    CC_LOG_INFO("{}: pack_half2x16 takes {} of {} inexact pairs toward zero on SGL's route, {} on DXC's",
                takes_dxil ? "dx12" : "vulkan", native_held.toward_zero, native_held.rounded, dxc_held.toward_zero);

    // the same operations on both routes, so every word alike to the bit; a NaN alike to any other NaN
    auto differs = 0;
    for (auto i = isize(0); i < values.size(); ++i)
        for (auto lane = 0; lane < 4; ++lane)
        {
            auto const is_half = lane >= 2;
            auto const a = is_half ? canonical_halves(through_native.packed[i][lane]) : through_native.packed[i][lane];
            auto const b = is_half ? canonical_halves(through_dxc.packed[i][lane]) : through_dxc.packed[i][lane];
            // what the SPIR-V route's byte ties, and every target's byte of a NaN, leave open
            auto const v = values[i];
            auto const is_open
                = !takes_dxil && !is_half
               && (is_nan(v[0]) || is_nan(v[1]) || is_nan(v[2]) || is_nan(v[3]) || is_byte_tie(v, lane == 1));
            if (a != b && !is_open && differs++ < 8)
                CHECK(a == b).dump("x", v).dump("lane", lane).dump("native", a).dump("dxc", b);
        }
    auto const same = [](f32 a, f32 b) { return bits_of(a) == bits_of(b) || (is_nan(a) && is_nan(b)); };
    for (auto i = isize(0); i < through_dxc.unpacked.size(); ++i)
        for (auto lane = 0; lane < 4; ++lane)
            if (!same(through_native.unpacked[i][lane], through_dxc.unpacked[i][lane]) && differs++ < 8)
                CHECK(false)
                    .dump("word", words[i / 3])
                    .dump("result", i % 3)
                    .dump("lane", lane)
                    .dump("native", through_native.unpacked[i][lane])
                    .dump("dxc", through_dxc.unpacked[i][lane]);
    CHECK(differs == 0);
}
