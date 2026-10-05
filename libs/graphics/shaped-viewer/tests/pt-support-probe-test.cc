#include "viewer_test_env.hh"

#include <clean-core/common/macros.hh>  // CC_ARCH_ARM64
#include <clean-core/common/utility.hh> // cc::clamp, cc::max
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/scene/light.hh>
#include <sv_test_shaders.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::dot, tg::normalize
#include <typed-geometry/scalar/scalar.hh>  // tg::abs, tg::sqrt

using namespace cc::primitive_defines;

// The path tracer's pure helpers in SGL, module `pt` and the lights and environment of module `scene`, run on the GPU.
//
// What is checked is what each helper promises: the random stream is PCG's to the bit, a sampled direction is
// unit and lies where its sampler says, a ray aimed at a rect lands at the distance it was aimed from, and the
// environment is the SH sum it is written as.
// `tests/shaders/pt_support_probe.sgl` writes eight float4s per item, in the order the checks below read them.
//
// The lights travel as `sv::shaders::tracer::light_record` itself, so this is also where `scene.light`'s layout meets the C++ struct.

namespace
{
static_assert(sizeof(sv_test::shaders::probe_light) == sizeof(sv::shaders::tracer::light_record),
              "probe_light in tests/shaders/pt_support_probe.sgl must match sv::shaders::tracer::light_record");

constexpr isize results_per_item = 8;
constexpr float pi = 3.14159265358979323846f;

/// The tracer's PCG step, on the CPU, which the GPU must reproduce to the bit.
u32 pcg_hash(u32 x)
{
    auto const s = x * 747796405u + 2891336453u;
    auto const w = ((s >> ((s >> 28) + 4u)) ^ s) * 277803737u;
    return (w >> 22) ^ w;
}

float pcg_rand(u32& state)
{
    state = state * 747796405u + 2891336453u;
    auto const w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    return float((w >> 22) ^ w) * (1.0f / 4294967296.0f);
}

/// `background_radiance` and `background_irradiance` as written, in float, for a reference within rounding.
tg::vec3f sh_radiance(cc::span<tg::vec4f const> sh, tg::vec3f d)
{
    auto const x = d[0];
    auto const y = d[1];
    auto const z = d[2];
    float const basis[16] = {
        0.282095f,
        0.488603f * y,
        0.488603f * z,
        0.488603f * x,
        1.092548f * x * y,
        1.092548f * y * z,
        0.315392f * (3.0f * z * z - 1.0f),
        1.092548f * x * z,
        0.546274f * (x * x - y * y),
        0.590044f * y * (3.0f * x * x - y * y),
        2.890611f * x * y * z,
        0.457046f * y * (5.0f * z * z - 1.0f),
        0.373176f * z * (5.0f * z * z - 3.0f),
        0.457046f * x * (5.0f * z * z - 1.0f),
        1.445306f * z * (x * x - y * y),
        0.590044f * x * (x * x - 3.0f * y * y),
    };
    auto sum = tg::vec3f(0, 0, 0);
    for (auto i = 0; i < 16; ++i)
        sum += tg::vec3f(sh[i][0], sh[i][1], sh[i][2]) * basis[i];
    return tg::vec3f(sum[0] > 0 ? sum[0] : 0.0f, sum[1] > 0 ? sum[1] : 0.0f, sum[2] > 0 ? sum[2] : 0.0f);
}

tg::vec3f sh_irradiance(cc::span<tg::vec4f const> sh, tg::vec3f n)
{
    auto const l = [&](int i) { return tg::vec3f(sh[i][0], sh[i][1], sh[i][2]); };
    auto const x = n[0];
    auto const y = n[1];
    auto const z = n[2];
    return 0.886227f * l(0) + 2.0f * 0.511664f * (l(3) * x + l(1) * y + l(2) * z) + 0.429043f * l(8) * (x * x - y * y)
         + 0.743125f * l(6) * (z * z) - 0.247708f * l(6)
         + 2.0f * 0.429043f * (l(4) * x * y + l(7) * x * z + l(5) * y * z);
}

bool near(float a, float b, float tolerance)
{
    return tg::abs(a - b) <= tolerance * (1.0f + tg::abs(b));
}

bool near(tg::vec3f a, tg::vec3f b, float tolerance)
{
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
}

/// Two rect lights over the unit square's double: one one-sided and plain, one two-sided, visible, shadowless and shaped.
cc::vector<sv::shaders::tracer::light_record> make_lights()
{
    auto lights = cc::vector<sv::shaders::tracer::light_record>();
    lights.push_back({.position = tg::vec3f(0, 0, 0),
                      .path = 1,
                      .u = tg::vec3f(1, 0, 0),
                      .area = 4.0f,
                      .v = tg::vec3f(0, 1, 0),
                      .emission = tg::vec3f(1, 2, 3),
                      .normal = tg::vec3f(0, 0, 1),
                      .cone_offset = 1,
                      .one_minus_cos_angular_radius = 0.01f,
                      .link_mask = ~0u});
    lights.push_back({.position = tg::vec3f(2, -1, 3),
                      .path = 1,
                      .u = tg::vec3f(0, 1, 0),
                      .area = 4.0f,
                      .v = tg::vec3f(0, 0, 1),
                      .flags = sv::light_flag_two_sided | sv::light_flag_visible_to_camera | sv::light_flag_casts_no_shadow,
                      .emission = tg::vec3f(4, 4, 4),
                      .cone_scale = 2.0f,
                      .normal = tg::vec3f(1, 0, 0),
                      .cone_offset = -0.5f,
                      .one_minus_cos_angular_radius = 1e-4f,
                      .link_mask = ~0u});
    return lights;
}

cc::vector<tg::vec4f> make_sh()
{
    auto sh = cc::vector<tg::vec4f>();
    for (auto i = 0; i < 16; ++i)
        sh.push_back(tg::vec4f(1.0f / float(i + 1), 0.25f - 0.03f * float(i), 0.1f * float(i % 4), 0.0f));
    return sh;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - the SGL path tracer helpers keep what each promises", (sg::context_handle const& ctx_h))
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the probe shader");

    auto const& entry = sv_test::shaders::pt_support_probe.pt_support_measure;
    auto const compiled_shader = entry->acquire(ctx);
    co_await cc::async_settled(compiled_shader);
    if (compiled_shader->has_error())
        FAIL(cc::format("the pt support probe did not compile:\n{}",
                        compiled_shader->try_error()->underlying().to_string()));
    auto const* const compiled = compiled_shader->try_value();
    REQUIRE(compiled != nullptr);

    auto const group_layout = ctx.cached.acquire_binding_group_layout<sv_test::shaders::pt_support_io>();
    auto pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = entry.acquire_layout(ctx)});
    auto const built = co_await pipeline;
    REQUIRE(built != nullptr);

    auto const lights = make_lights();
    auto const sh = make_sh();
    auto const item_count = isize(256);

    auto cmd = ctx.create_command_list();

    auto const light_buffer = ctx.transient.create_buffer<sv::shaders::tracer::light_record>(
        lights.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(light_buffer, cc::span<sv::shaders::tracer::light_record const>(lights));
    auto const sh_buffer = ctx.transient.create_buffer<tg::vec4f>(
        sh.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(sh_buffer, cc::span<tg::vec4f const>(sh));
    auto const result_buffer = ctx.transient.create_buffer<tg::vec4f>(
        item_count * results_per_item, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto const group = ctx.transient.create_binding_group(
        *cmd, group_layout,
        sv_test::shaders::pt_support_io{
            .lights = light_buffer.reinterpret_as<sv_test::shaders::probe_light>().as_readonly_buffer(),
            .sh = sh_buffer.as_readonly_buffer(),
            .results = result_buffer.as_readwrite_buffer(),
            .item_count = u32(item_count),
            .light_count = u32(lights.size())});

    cmd->compute.bind_pipeline(*built);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(item_count);
    auto readback = cmd->download.data_from_buffer(result_buffer);
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const r = co_await readback.data();
    REQUIRE(r.size() == item_count * results_per_item);

    auto const select_pdf = 1.0f / float(lights.size());
    auto landed_count = 0;
    auto missed_count = 0;

    for (auto item = isize(0); item < item_count; ++item)
    {
        auto const& l = lights[item % lights.size()];
        auto const* const w = r.data() + item * results_per_item;
        auto const where = [&](char const* what) { return cc::format("item {}: {}", item, what); };

        auto rng = pcg_hash(u32(item) * 9781u + 1u);
        auto const u1 = pcg_rand(rng);
        auto const u2 = pcg_rand(rng);
        auto const u3 = pcg_rand(rng);
        auto const u4 = pcg_rand(rng);
        auto const u5 = pcg_rand(rng);
        auto const n = tg::normalize(tg::vec3f(u1, u2, u3) - tg::vec3f(0.5f, 0.5f, 0.5f) + tg::vec3f(0, 0, 1e-3f));

        // The stream is PCG's, and the weights, the environment pdf and the selection pdf are what they say.
        // A 32-bit word does not fit a float, and D3D lets the conversion round either way — WARP and hardware differ —
        // so the uniform is within one float step; a wrong word lands far further off.
        auto const ulps = i64(cc::bit_cast<u32>(w[0][0])) - i64(cc::bit_cast<u32>(u1));
        auto const is_within_one_step = ulps >= -1 && ulps <= 1;
        CHECK(is_within_one_step).context(where("the first uniform"));
        CHECK(near(w[0][1], 1.0f, 1e-6f)).context(where("the two MIS weights of one pair sum to one"));
        CHECK(near(w[0][2], 1.0f / (2.0f * pi), 1e-6f)).context(where("the environment pdf"));
        CHECK(near(w[0][3], select_pdf, 1e-6f)).context(where("the light selection pdf"));

        // A cosine-weighted direction is unit, finite and in n's hemisphere.
        auto const cosine = tg::vec3f(w[1][0], w[1][1], w[1][2]);
        CHECK(near(cosine.length(), 1.0f, 1e-5f)).context(where("the cosine sample is unit"));
        CHECK(w[1][3] == 1.0f).context(where("the cosine sample is finite"));
        CHECK(tg::dot(cosine, n) >= -1e-4f).context(where("the cosine sample lies about n"));

        // A Henyey-Greenstein direction is unit, and the phase function along it is the density it was drawn from.
        auto const g = u3 * 1.8f - 0.9f;
        auto const hg = tg::vec3f(w[2][0], w[2][1], w[2][2]);
        auto const mu = tg::dot(n, hg);
        auto const g2 = g * g;
        auto const d = 1.0f + g2 - 2.0f * g * mu;
        auto const phase = (1.0f - g2) / (4.0f * pi * d * tg::sqrt(d));
        CHECK(near(hg.length(), 1.0f, 1e-5f)).context(where("the phase sample is unit"));
        CHECK(near(w[2][3], phase, 2e-3f)).context(where("the phase function along the sample"));

        // A cone sample lies inside the disc it was drawn for, which `in_disc` agrees with away from the rim.
        auto const toward = -l.normal;
        auto const cone = tg::vec3f(w[3][0], w[3][1], w[3][2]);
        CHECK(1.0f - tg::dot(cone, toward) <= l.one_minus_cos_angular_radius * 1.01f + 1e-6f)
            .context(where("the cone sample"));
        if (u4 < 0.98f)
            CHECK(w[3][3] == 1.0f).context(where("in_disc agrees the cone sample is inside"));

        // Aimed straight down from 5 above the rect, at in-plane coordinates (s, t) the rect spans when both lie in [-1, 1].
        auto const s = (u1 - 0.5f) * 4.0f;
        auto const t = (u2 - 0.5f) * 4.0f;
        auto const clearly_in = tg::abs(s) < 0.999f && tg::abs(t) < 0.999f;
        auto const clearly_out = tg::abs(s) > 1.001f || tg::abs(t) > 1.001f;
        if (clearly_in)
        {
            ++landed_count;
            CHECK(w[4][0] == 1.0f).context(where("a ray aimed inside the rect lands"));
            CHECK(near(w[4][1], 5.0f, 1e-5f)).context(where("at the distance it was aimed from"));
            CHECK(near(w[4][2], 1.0f, 1e-6f)).context(where("head on"));
            CHECK(near(w[4][3], select_pdf * 25.0f / l.area, 1e-5f)).context(where("the rect's solid-angle density"));
        }
        if (clearly_out)
        {
            ++missed_count;
            CHECK(w[4][0] == 0.0f).context(where("a ray aimed beside the rect misses"));
            CHECK(w[4][1] == 0.0f).context(where("and leaves its distance zeroed"));
            CHECK(w[4][2] == 0.0f).context(where("and its cosine"));
        }

        // The environment is the SH sum it is written as, and the disc density is uniform over the disc's cone.
        CHECK(near(tg::vec3f(w[5][0], w[5][1], w[5][2]), sh_radiance(sh, n), 1e-4f)).context(where("the SH radiance"));
        CHECK(near(w[5][3], select_pdf / (2.0f * pi * l.one_minus_cos_angular_radius), 1e-5f)).context(where("the disc pdf"));
        CHECK(near(tg::vec3f(w[6][0], w[6][1], w[6][2]), sh_irradiance(sh, n), 1e-4f)).context(where("the SH irradiance"));

        auto const cone_a = cc::clamp(u1 * l.cone_scale + l.cone_offset, 0.0f, 1.0f);
        CHECK(near(w[6][3], cone_a * cone_a, 1e-6f)).context(where("the cone falloff"));

        auto const shadows = (l.flags & sv::light_flag_casts_no_shadow) == 0;
        auto const visible = (l.flags & sv::light_flag_visible_to_camera) != 0;
        CHECK(w[7][0] == (shadows ? 1.0f : 0.0f)).context(where("light_casts_shadows reads the flag through the layout"));
        CHECK(w[7][1] == (visible ? 1.0f : 0.0f)).context(where("light_visible_to_camera reads the flag through the layout"));
        CHECK(w[7][2] == 16.0f).context(where("roulette_after"));
        CHECK(w[7][3] == 4096.0f).context(where("scatter_cap"));
    }

    // Both branches of the rect test were taken, so neither set of checks above was vacuous.
    CHECK(landed_count > 0);
    CHECK(missed_count > 0);
}
