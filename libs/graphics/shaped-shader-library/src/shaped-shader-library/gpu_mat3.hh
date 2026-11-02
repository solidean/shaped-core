#pragma once

#include <clean-core/container/fixed_array.hh>
#include <shaped-shader-library/fwd.hh>
#include <shaped-shader-library/gpu_padded.hh>
#include <typed-geometry/linalg/mat.hh>

/// A `mat3` as GPU memory holds one: three columns, 16 bytes apart, 48 bytes in all.
/// SGL places a 3-vector at 16, so a matrix of three of them takes this shape; `tg::mat3f` is the same columns packed
/// into 36, which a shader asks for with `@padding(.none)`.
/// It converts to and from `tg::mat3f`, so a host fills it from a matrix and reads one back.
struct slib::gpu_mat3
{
    cc::fixed_array<gpu_padded<tg::vec3f, 16>, 3> cols = {};

    gpu_mat3() = default;
    constexpr gpu_mat3(tg::mat3f const& m) : cols{m.cols[0], m.cols[1], m.cols[2]} {}

    [[nodiscard]] constexpr operator tg::mat3f() const
    {
        return tg::mat3f::make_from_cols(cols[0].value, cols[1].value, cols[2].value);
    }
};

namespace slib
{
static_assert(sizeof(gpu_mat3) == 48, "gpu_mat3 must be three 16-byte columns");
} // namespace slib
