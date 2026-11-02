#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/primitives/quad.hh>
#include <typed-geometry/geometry/primitives/segment.hh>

// The edges and faces of a parallelotope given by its corners, shared by aabb, box and frustum.
// Corner c sits on the far side of axis i iff bit i of c is set, which is how all three number their vertices().

namespace tg::impl
{
/// Every pair of corners that differ in exactly one bit: D · 2^(D-1) edges, those along axis 0 first within each corner.
template <int D, int DAmbient, class T>
[[nodiscard]] constexpr cc::fixed_array<segment<DAmbient, T>, D * (1 << (D - 1))> corner_edges(
    cc::fixed_array<pos<DAmbient, T>, (1 << D)> const& v)
{
    cc::fixed_array<segment<DAmbient, T>, D * (1 << (D - 1))> r = {};
    auto n = 0;
    for (int c = 0; c < (1 << D); ++c)
        for (int i = 0; i < D; ++i)
            if (!((c >> i) & 1))
                r[n++] = segment<DAmbient, T>(v[c], v[c | (1 << i)]);
    return r;
}

/// The six faces of a hexahedron, face 2a + s being the one whose corners have bit a equal to s.
/// Each is wound counterclockwise seen from outside when axes 0, 1, 2 form a right-handed frame; `mirrored` says they form a left-handed one.
template <class T>
[[nodiscard]] constexpr cc::fixed_array<quad<3, T>, 6> corner_faces(cc::fixed_array<pos<3, T>, 8> const& v, bool mirrored)
{
    cc::fixed_array<quad<3, T>, 6> r = {};
    for (int a = 0; a < 3; ++a)
    {
        auto const u = (a + 1) % 3;
        auto const w = (a + 2) % 3;
        for (int s = 0; s < 2; ++s)
        {
            auto const at = [&](int bu, int bw) { return v[(s << a) | (bu << u) | (bw << w)]; };
            // (u, w, a) is a cyclic permutation of (0, 1, 2), so u × w points along +a: the far face takes u then w
            auto const along_u_first = (s == 1) != mirrored;
            r[2 * a + s] = along_u_first ? quad<3, T>(at(0, 0), at(1, 0), at(1, 1), at(0, 1))
                                         : quad<3, T>(at(0, 0), at(0, 1), at(1, 1), at(1, 0));
        }
    }
    return r;
}
} // namespace tg::impl
