#pragma once

#include <clean-core/container/fixed_array.hh>
#include <shaped-shader-library/fwd.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// An `affine3` as GPU memory holds one: its three rows of four, 16 bytes apart, 48 bytes in all (CHK-518).
/// The same shape an instance's transform takes in a ray-tracing scene, so either can fill the other.
/// A row is `(linear row, translation)`: the fourth lane of row `i` is the translation's `i`th coordinate.
struct slib::gpu_affine3
{
    cc::fixed_array<tg::vec4f, 3> rows = {tg::vec4f(1, 0, 0, 0), tg::vec4f(0, 1, 0, 0), tg::vec4f(0, 0, 1, 0)};

    gpu_affine3() = default;
    constexpr gpu_affine3(tg::vec4f r0, tg::vec4f r1, tg::vec4f r2) : rows{r0, r1, r2} {}

    /// The map's top three rows; an affine transform's last row is (0, 0, 0, 1), which GPU memory leaves out.
    gpu_affine3(tg::affine_transform3f const& t)
    {
        auto const m = t.to_mat();
        for (auto r = 0; r < 3; ++r)
            rows[r] = tg::vec4f(m[0, r], m[1, r], m[2, r], m[3, r]);
    }
};

namespace slib
{
static_assert(sizeof(gpu_affine3) == 48, "gpu_affine3 must be three 16-byte rows");
} // namespace slib
