#include <shaped-viewer/scene/background.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::normalize

namespace sv
{
namespace
{
// The real-SH basis normalizations, the same numbers shaders/background.hlsli evaluates.
// A factory that wants radiance `c` along a basis function stores `c / k`, because the miss multiplies the coefficient back by `k`.
constexpr f32 k_y00 = 0.2820948f; // Y(0,0), the constant term
constexpr f32 k_y1 = 0.4886025f;  // Y(1,-1)/y, Y(1,0)/z, Y(1,1)/x
constexpr f32 k_y2 = 1.0925484f;  // Y(2,-2)/xy, Y(2,-1)/yz, Y(2,1)/xz
constexpr f32 k_y20 = 0.3153916f; // Y(2,0)/(3z^2 - 1)
constexpr f32 k_y22 = 0.5462742f; // Y(2,2)/(x^2 - y^2)

// Zonal coefficients of the clamped cosine max(0, cos t), already folded with the sqrt(4pi / (2l + 1)) of the
// zonal-harmonics rotation — so a lobe aimed at `d` is just `zh_l * Y_lm(d)`. Band 3 is exactly zero, band 4 is not:
// this is the standard truncation, not an exact representation.
constexpr f32 zh_cos0 = 3.1415927f; // pi
constexpr f32 zh_cos1 = 2.0943951f; // 2 pi / 3
constexpr f32 zh_cos2 = 0.7853982f; // pi / 4

// The truncated lobe overshoots: it reconstructs 17/16 along its own axis, where the clamped cosine is 1.
// Dividing it out is what makes `sun`'s peak the radiance the caller asked for.
constexpr f32 zh_cos_peak = 16.0f / 17.0f;
} // namespace

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

background background::sun(tg::vec3f direction, tg::vec3f radiance)
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

background background::daylight()
{
    return gradient(tg::vec3f(0.55f, 0.78f, 1.30f), tg::vec3f(0.26f, 0.22f, 0.18f))
        .combined_with(sun(tg::vec3f(0.6f, 0.7f, 0.4f), tg::vec3f(0.90f, 0.78f, 0.60f)));
}

background background::studio()
{
    return gradient(tg::vec3f(0.90f, 0.90f, 0.90f), tg::vec3f(0.25f, 0.25f, 0.25f));
}

background background::combined_with(background const& other) const
{
    auto out = background{};
    for (auto i = 0; i < sh_coefficient_count; ++i)
        out.sh[i] = sh[i] + other.sh[i];
    return out;
}

background background::scaled(f32 factor) const
{
    auto out = background{};
    for (auto i = 0; i < sh_coefficient_count; ++i)
        out.sh[i] = sh[i] * factor;
    return out;
}
} // namespace sv
