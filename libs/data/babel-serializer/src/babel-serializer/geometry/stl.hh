#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

// STL reader (geometry/), for both containers.
//
// A faithful, flat mirror of the file: three positions per triangle in file order, plus whatever normal each facet
// declared.
// NO welding, NO indexing, NO normal derivation — STL is a soup of independent triangles and this reader keeps it one.
// Building a mesh out of that belongs to whoever wants a mesh.
//
// STL carries no materials, no uvs and no hierarchy, so there is nothing else to mirror.
//
//   auto const m = babel::stl::read(bytes).value();
//   for (auto t = isize(0); t < m.triangle_count(); ++t)
//       use(m.positions[t * 3], m.positions[t * 3 + 1], m.positions[t * 3 + 2]);

/// Which container the bytes were in.
enum class babel::stl::container : babel::u8
{
    ascii,
    binary,
};

/// The faithful parse of an .stl.
/// Read-once; `positions` mirrors the file's triangle order, three entries at a time.
struct babel::stl::data
{
    container source = container::binary;

    /// the `solid` name an ascii file opened with; always empty for a binary one, whose 80-byte header is free-form
    /// vendor text rather than a name
    cc::string name;

    /// One per triangle, exactly as written.
    /// A zero normal is the file saying "derive it from the winding", which is common and not an error — deriving is
    /// the caller's, since this reader computes nothing.
    cc::vector<tg::vec3f> normals;

    /// 3 per triangle, in file order
    cc::vector<tg::pos3f> positions;

    /// Binary only: the per-triangle "attribute byte count", which the format reserves and some exporters abuse to
    /// store a color.
    /// Empty for an ascii file, which has no such field.
    cc::vector<u16> attribute_counts;

    [[nodiscard]] isize triangle_count() const { return positions.size() / 3; }
    [[nodiscard]] bool is_empty() const { return positions.empty(); }
};

namespace babel::stl
{

// reading
// -------------------------------------------------------------------------------------------------

/// Which container `bytes` is in.
///
/// The SIZE is what decides it, not the leading `solid`: a binary file's 80-byte header is free-form and plenty of
/// exporters write "solid" into it, which is the trap this exists to avoid.
/// A binary file is exactly `84 + 50 * triangle_count` bytes, and nothing else is treated as one.
[[nodiscard]] container detect_container(cc::span<byte const> bytes);

/// Parse an .stl, auto-detecting the container.
///
/// Fails on a malformed ascii facet, with a line-numbered error.
/// Fails too on a binary file whose last record is cut short, rather than letting it fall through to the ascii parse
/// and come back as an empty `solid` — which is the one failure mode nobody would think to check for.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes);

/// Convenience for an ascii document already in hand.
[[nodiscard]] cc::result<data> read(cc::string_view text);

/// Convenience: slurps the stream, then parses.
/// Slurped rather than streamed because the container is decided by the total size, which a stream only knows once it
/// has been read.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);
} // namespace babel::stl
