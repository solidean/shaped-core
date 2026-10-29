#include <shaped-rendering/display.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::pow

namespace sr
{
f32 display_transform::exposure_scale() const
{
    return tg::pow(2.0f, exposure);
}
} // namespace sr
