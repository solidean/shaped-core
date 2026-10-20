#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// What a texture's texels or a mesh attribute's elements hold, per channel: their mean, and the range they span.
///
/// Read once on the CPU while the payload is at hand, and kept with the resource for an emissive instance's selection weight.
/// Selection only shapes the noise, so an estimate serves: a scan that skipped values averages only those it read.
/// `min` and `max` bound every value only where `exact` holds, which is what may prove an emission dark.
/// A summary of anything but the very values the sampler reads is never exact, a lossy re-encoding of them included.
struct sv::value_summary
{
    tg::vec4f mean = tg::vec4f(0, 0, 0, 0);
    tg::vec4f min = tg::vec4f(0, 0, 0, 0);
    tg::vec4f max = tg::vec4f(0, 0, 0, 0);

    /// Whether every value the sampler reads was read, so `min` and `max` bound them all.
    bool exact = false;
};

namespace sv
{
/// The summary of one texture level, `width` x `height` texels of `format` in `level_bytes`, as the sampler returns them.
///
/// An sRGB format is decoded to linear, a channel the format lacks reads 0, and a missing alpha 1.
/// A block-compressed format is decoded by babel::bcn, and its summary is never exact, since hardware decodes it only to a tolerance.
/// Empty for a format it does not decode: an integer, an uncompressed snorm or a packed one.
/// A level of more than `max_texels` texels is read on a coarser grid of short row runs, so its mean is an estimate and the summary not exact.
[[nodiscard]] cc::optional<value_summary> summarize_texels(cc::span<byte const> level_bytes,
                                                           sg::pixel_format format,
                                                           i32 width,
                                                           i32 height,
                                                           isize max_texels = 65536);

/// The summary of `texture`'s smallest supplied level, the cheapest to read whole, as `texture_record::summary` keeps it.
/// Exact only where that level is the base, read whole: a smaller level's range need not bound the base's.
[[nodiscard]] cc::optional<value_summary> summarize_texture(texture_data const& texture);

/// The summary of `count` elements of `format`, packed from offset 0 in `bytes`.
/// Only an f32 scalar or vector of up to four lanes is summarized, the lanes past it reading 0; any other format is empty.
/// More than `max_elements` elements are read in short runs on a coarser grid, as a large texture level is, so the summary is not exact.
[[nodiscard]] cc::optional<value_summary> summarize_elements(cc::span<byte const> bytes,
                                                             attribute_format format,
                                                             isize count,
                                                             isize max_elements = 65536);
} // namespace sv
