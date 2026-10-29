#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::clamp, cc::max
#include <shaped-viewer/scene/background.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::normalize
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::exp

namespace sv
{
namespace
{
// The real-SH basis normalizations, the same numbers `scene.background_radiance` (shaders/sgl/scene_items.sgl) evaluates.
// A factory that wants radiance `c` along a basis function stores `c / k`, because the miss multiplies the coefficient back by `k`.
constexpr f32 k_y00 = 0.2820948f; // Y(0,0), the constant term
constexpr f32 k_y1 = 0.4886025f;  // Y(1,-1)/y, Y(1,0)/z, Y(1,1)/x
constexpr f32 k_y2 = 1.0925484f;  // Y(2,-2)/xy, Y(2,-1)/yz, Y(2,1)/xz
constexpr f32 k_y20 = 0.3153916f; // Y(2,0)/(3z^2 - 1)
constexpr f32 k_y22 = 0.5462742f; // Y(2,2)/(x^2 - y^2)

// Zonal coefficients of the clamped cosine max(0, cos t), already folded with the sqrt(4pi / (2l + 1)) of the
// zonal-harmonics rotation — so a lobe aimed at `d` is just `zh_l * Y_lm(d)`.
// Band 3 is exactly zero, band 4 is not:
// this is the standard truncation, not an exact representation.
constexpr f32 zh_cos0 = 3.1415927f; // pi
constexpr f32 zh_cos1 = 2.0943951f; // 2 pi / 3
constexpr f32 zh_cos2 = 0.7853982f; // pi / 4

// The truncated lobe overshoots: it reconstructs 17/16 along its own axis, where the clamped cosine is 1.
// Dividing it out is what makes `lobe`'s peak the radiance the caller asked for.
constexpr f32 zh_cos_peak = 16.0f / 17.0f;

/// The 16 real-SH basis values along unit direction `d`, as `scene.background_radiance` evaluates them.
void sh_basis(tg::vec3f d, f32 (&y)[background::sh_coefficient_count])
{
    auto const x = d[0];
    auto const yy = d[1];
    auto const z = d[2];
    y[0] = 0.282095f;
    y[1] = 0.488603f * yy;
    y[2] = 0.488603f * z;
    y[3] = 0.488603f * x;
    y[4] = 1.092548f * x * yy;
    y[5] = 1.092548f * yy * z;
    y[6] = 0.315392f * (3.0f * z * z - 1.0f);
    y[7] = 1.092548f * x * z;
    y[8] = 0.546274f * (x * x - yy * yy);
    y[9] = 0.590044f * yy * (3.0f * x * x - yy * yy);
    y[10] = 2.890611f * x * yy * z;
    y[11] = 0.457046f * yy * (5.0f * z * z - 1.0f);
    y[12] = 0.373176f * z * (5.0f * z * z - 3.0f);
    y[13] = 0.457046f * x * (5.0f * z * z - 1.0f);
    y[14] = 1.445306f * z * (x * x - yy * yy);
    y[15] = 0.590044f * x * (x * x - 3.0f * yy * yy);
}

/// BT.709 luminance, the weight a sample's share of the dome is chosen by.
[[nodiscard]] f32 luminance(tg::vec3f c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/// The integral of a lobe of unit peak, `exp(k (cos t - 1))`, over the sphere.
[[nodiscard]] f32 vmf_integral(f32 k)
{
    return 2.0f * tg::pi<f32> * (1.0f - tg::exp(-2.0f * k)) / k;
}
} // namespace

tg::vec3f dome::gradient(tg::vec3f d) const
{
    auto const t = cc::clamp((d[1] - rise_start) / cc::max(1.0f - rise_start, 1e-6f), 0.0f, 1.0f);
    return horizon + (zenith - horizon) * (t * t * (3.0f - 2.0f * t));
}

tg::vec3f dome::radiance(tg::vec3f d) const
{
    auto out = gradient(d);
    for (auto i = 0; i < lobe_count; ++i)
    {
        auto const& l = lobes[i];
        out += l.radiance * tg::exp(l.concentration * (tg::dot(d, tg::normalize(l.direction)) - 1.0f));
    }
    return out;
}

background background::of_dome(sv::dome const& d)
{
    CC_ASSERT(d.lobe_count >= 0 && d.lobe_count <= dome::max_lobes, "a dome holds at most max_lobes lobes");
    auto bg = background{.is_dome = true, .analytic = d};

    // Projected by quadrature over equal-angle cells, each weighed by its solid angle: what a reader of SH alone sees of the dome.
    constexpr int rows = 64;
    constexpr int columns = 128;
    for (auto r = 0; r < rows; ++r)
    {
        auto const theta = tg::angle_f::make_from_radians((f32(r) + 0.5f) / f32(rows) * tg::pi<f32>);
        auto const weight = theta.sin() * (tg::pi<f32> / f32(rows)) * (2.0f * tg::pi<f32> / f32(columns));
        for (auto c = 0; c < columns; ++c)
        {
            auto const phi = tg::angle_f::make_from_radians((f32(c) + 0.5f) / f32(columns) * 2.0f * tg::pi<f32>);
            auto const dir = tg::vec3f(theta.sin() * phi.cos(), theta.cos(), theta.sin() * phi.sin());
            auto const radiance = d.radiance(dir);
            f32 y[sh_coefficient_count] = {};
            sh_basis(dir, y);
            for (auto i = 0; i < sh_coefficient_count; ++i)
                bg.sh[i] += radiance * (y[i] * weight);
        }
    }
    return bg;
}

background_gpu background_gpu::from(background const& bg)
{
    auto out = background_gpu{};
    for (auto i = 0; i < background::sh_coefficient_count; ++i)
        out.sh[i] = tg::vec4f(bg.sh[i][0], bg.sh[i][1], bg.sh[i][2], 0.0f);
    if (!bg.is_dome)
        return out;

    auto const& d = bg.analytic;
    out.header = tg::vec4f(1.0f, f32(d.lobe_count), d.rise_start, d.backdrop_scale);
    out.horizon = tg::vec4f(d.horizon[0], d.horizon[1], d.horizon[2], 0.0f);
    out.zenith = tg::vec4f(d.zenith[0], d.zenith[1], d.zenith[2], 0.0f);

    // Each part's share of the samples follows its power: a lobe's peak times its integral, the gradient's mean times the sphere.
    // The gradient keeps at least a fifth, so the dome between the lobes is never starved of samples.
    f32 power[dome::max_lobes] = {};
    auto lobes_total = 0.0f;
    for (auto i = 0; i < d.lobe_count; ++i)
    {
        power[i] = luminance(d.lobes[i].radiance) * vmf_integral(d.lobes[i].concentration);
        lobes_total += power[i];
    }
    auto const gradient_power = luminance(d.horizon + d.zenith) * 0.5f * 4.0f * tg::pi<f32>;
    auto const total = lobes_total + gradient_power;
    auto const gradient_share = total > 0.0f ? cc::max(gradient_power / total, lobes_total > 0.0f ? 0.2f : 1.0f) : 1.0f;
    auto const lobe_scale = lobes_total > 0.0f ? (1.0f - gradient_share) / lobes_total : 0.0f;
    for (auto i = 0; i < d.lobe_count; ++i)
    {
        auto const& l = d.lobes[i];
        auto const dir = tg::normalize(l.direction);
        out.lobe_direction[i] = tg::vec4f(dir[0], dir[1], dir[2], l.concentration);
        out.lobe_radiance[i] = tg::vec4f(l.radiance[0], l.radiance[1], l.radiance[2], power[i] * lobe_scale);
    }
    out.gradient = tg::vec4f(gradient_share, 0.0f, 0.0f, 0.0f);
    return out;
}

background background::uniform(tg::vec3f radiance)
{
    auto bg = background{};
    bg.sh[0] = radiance / k_y00;
    return bg;
}

background background::gradient(tg::vec3f zenith, tg::vec3f nadir)
{
    // L(d) = mean + slope * d.y, which is exactly band 0 plus the Y(1,-1) lane.
    auto const mean = (zenith + nadir) * 0.5f;
    auto const slope = (zenith - nadir) * 0.5f;

    auto bg = background{};
    bg.sh[0] = mean / k_y00;
    bg.sh[1] = slope / k_y1;
    return bg;
}

background background::lobe(tg::vec3f direction, tg::vec3f radiance)
{
    auto const d = tg::normalize(direction);
    auto const x = d[0];
    auto const y = d[1];
    auto const z = d[2];

    auto const c = radiance * zh_cos_peak;

    auto bg = background{};
    bg.sh[0] = c * (zh_cos0 * k_y00);

    bg.sh[1] = c * (zh_cos1 * k_y1 * y);
    bg.sh[2] = c * (zh_cos1 * k_y1 * z);
    bg.sh[3] = c * (zh_cos1 * k_y1 * x);

    bg.sh[4] = c * (zh_cos2 * k_y2 * x * y);
    bg.sh[5] = c * (zh_cos2 * k_y2 * y * z);
    bg.sh[6] = c * (zh_cos2 * k_y20 * (3.0f * z * z - 1.0f));
    bg.sh[7] = c * (zh_cos2 * k_y2 * x * z);
    bg.sh[8] = c * (zh_cos2 * k_y22 * (x * x - y * y));
    return bg;
}

background background::studio()
{
    return gradient(tg::vec3f(0.90f, 0.90f, 0.90f), tg::vec3f(0.25f, 0.25f, 0.25f));
}

background background::combined_with(background const& other) const
{
    CC_ASSERT(!is_dome && !other.is_dome, "a dome composes with nothing: it is the environment, and its SH only its "
                                          "projection");
    auto out = background{};
    for (auto i = 0; i < sh_coefficient_count; ++i)
        out.sh[i] = sh[i] + other.sh[i];
    return out;
}

background background::scaled(f32 factor) const
{
    auto out = *this;
    for (auto i = 0; i < sh_coefficient_count; ++i)
        out.sh[i] = sh[i] * factor;
    out.analytic.horizon = analytic.horizon * factor;
    out.analytic.zenith = analytic.zenith * factor;
    for (auto i = 0; i < analytic.lobe_count; ++i)
        out.analytic.lobes[i].radiance = analytic.lobes[i].radiance * factor;
    return out;
}

sky_and_sun daylight()
{
    // The sun the old soft lobe stood for: its warm tint, and the illuminance it delivered to a surface facing it.
    // A clamped-cosine lobe of peak radiance P gives 2 pi / 3 * P there, so its 0.9 peak becomes about 1.9 lux.
    auto const toward_sun = tg::vec3f(0.6f, 0.7f, 0.4f);
    auto sun = light::sun(-toward_sun).lux(0.90f * 2.0f * tg::pi<f32> / 3.0f);
    sun.color(tg::vec3f(1.0f, 0.78f / 0.90f, 0.60f / 0.90f));

    return {.sky = background::gradient(tg::vec3f(0.55f, 0.78f, 1.30f), tg::vec3f(0.26f, 0.22f, 0.18f)), .sun = sun};
}
} // namespace sv
