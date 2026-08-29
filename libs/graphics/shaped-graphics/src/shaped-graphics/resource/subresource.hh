#pragma once

#include <clean-core/common/hash.hh>    // cc::make_hash (subresource_range's hidden friend)
#include <clean-core/common/utility.hh> // cc::start_end
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh> // format_aspect_at maps an index onto a format's planes

/// Subresource addressing for textures.
/// A texture's subresource domain is the discrete grid of (mip level × array slice × aspect plane).
/// Buffers are single-subresource and never use any of this.
///
/// Pure addressing vocabulary: the access state kept per subresource lives in `barrier/subresource_state.hh`, which builds its covering partition on top of these ranges.

/// Which plane of a possibly multi-planar texture a subresource addresses.
/// Single-plane formats use `color`, or `depth`; depth-stencil and video formats expose several planes.
enum class sg::texture_aspect : sg::u32
{
    color,
    depth,
    stencil,
    plane0,
    plane1,
    plane2,
};

/// The size of a texture's subresource domain along each axis.
/// `aspect_count` is the number of planes — 1 for a plain color texture, 2 for depth+stencil.
/// A buffer is `{1, 1, 1}`.
struct sg::subresource_extent
{
    int mip_count = 1;
    int array_count = 1;
    int aspect_count = 1;

    [[nodiscard]] int total() const { return mip_count * array_count * aspect_count; }
};

namespace sg
{
/// The aspect plane at `index` within a format's own subresource domain.
///
/// The index is POSITIONAL, not a `texture_aspect` value: plane 0 is `color` for a color format and `depth` for a
/// depth one, and only a combined depth-stencil format has a plane 1.
/// That is what makes `format_aspect_count` a count rather than a bit set — and casting the index straight to
/// `texture_aspect` names the color plane of a depth texture, which every graphics API rejects.
[[nodiscard]] constexpr texture_aspect format_aspect_at(pixel_format f, int index)
{
    if (!is_depth_format(f))
        return texture_aspect::color;
    return index == 0 ? texture_aspect::depth : texture_aspect::stencil;
}
} // namespace sg

/// Addresses a single subresource: one (mip level, array layer, aspect) point in the grid — the point analog of `subresource_range`.
/// Defaults to the first subresource: mip 0, layer 0, color.
/// `array_layer` counts slices, so a cube's faces are layers 0..5 and a cube array's are `6*cube + face`.
struct sg::subresource_index
{
    int mip_level = 0;
    int array_layer = 0;
    texture_aspect aspect = texture_aspect::color;
};

/// A half-open box in the subresource grid: a `[start, end)` range on each of the mip, array-slice and aspect-plane axes.
/// Used to name the range an access covers.
/// A single `subresource_index` converts to the one-wide box at its point, so an op touching one subresource can pass it directly.
struct sg::subresource_range
{
    cc::start_end mip_range = {.start = 0, .end = 1};
    cc::start_end array_range = {.start = 0, .end = 1};
    cc::start_end aspect_range = {.start = 0, .end = 1};

    subresource_range() = default;
    subresource_range(cc::start_end mips, cc::start_end arrays, cc::start_end aspects)
      : mip_range(mips), array_range(arrays), aspect_range(aspects)
    {
    }
    subresource_range(subresource_index const& sub) // NOLINT(*-explicit-*): a single subresource *is* a one-wide range
      : mip_range{.start = sub.mip_level, .end = sub.mip_level + 1},
        array_range{.start = sub.array_layer, .end = sub.array_layer + 1},
        aspect_range{.start = int(sub.aspect), .end = int(sub.aspect) + 1}
    {
    }

    [[nodiscard]] static subresource_range whole(subresource_extent e)
    {
        return {{.start = 0, .end = e.mip_count}, {.start = 0, .end = e.array_count}, {.start = 0, .end = e.aspect_count}};
    }
    [[nodiscard]] bool is_empty() const
    {
        return mip_range.start >= mip_range.end || array_range.start >= array_range.end
            || aspect_range.start >= aspect_range.end;
    }

    /// Structural hash over the three ranges' bounds (the cc::make_hash protocol's hidden friend).
    [[nodiscard]] friend u64 hash(subresource_range const& r)
    {
        return cc::make_hash(r.mip_range.start, r.mip_range.end, r.array_range.start, r.array_range.end,
                             r.aspect_range.start, r.aspect_range.end);
    }

    /// Structural equality over the same three ranges `hash` above folds — a hash map keyed on a range needs
    /// the two to agree, and defaulting it is what keeps them agreeing as fields are added.
    [[nodiscard]] friend bool operator==(subresource_range const&, subresource_range const&) = default;
};
