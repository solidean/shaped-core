#include <shaped-rendering/display.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::pow, tg::log

namespace sr
{
f32 display_transform::exposure_scale() const
{
    return tg::pow(2.0f, exposure);
}

f32 exposure_for_average(f32 average)
{
    return tg::log(0.18f / average) / tg::log(2.0f);
}
} // namespace sr
