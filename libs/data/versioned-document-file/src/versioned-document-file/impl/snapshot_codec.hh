#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document/snapshot_document.hh>

/// How a materialized document becomes the bytes of a `snapshots` row, and back.
///
/// A snapshot holds `surviving` and nothing else, so this encodes exactly a raw_document.
/// The argument for that shape is
/// [decisions.md](../../../../versioned-document/docs/decisions.md#a-snapshot-stores-surviving-only-and-its-validity-is-decided-at-use);
/// the on-disk layout is [format.md](../../../docs/format.md#snapshots--materialization-caches).
///
/// **Four intern tables, and they are what makes this affordable.**
/// A document of a few million properties would spend ~32 bytes per property on writer ids alone, but the number of
/// distinct writer ops is far smaller than the number of properties, since one op writes many paths.
/// Property names repeat the same way: nesting already dedups entity and component names, while `"position"` recurs
/// once per (entity, component).
///
/// The encoding is CANONICAL and the decoder enforces it — every table and every level ascending by canonical bytes,
/// deduplicated.
/// Same posture as vdoc::try_decode: repair nothing, reject non-canonical input.
/// Two builds computing the same snapshot then produce byte-identical rows.
///
/// **Every value carries its own extent**, exactly as an assignment does.
/// vdoc::try_decode rejects trailing bytes, so a value can only be validated against a slice already known to be
/// exactly it, and vdoc::encoded_size may not be asked before that validation has happened.

namespace vdoc::file::impl
{
/// The snapshot encoding this build writes.
/// Versioned in the name so a `snapshot-v2` can coexist rather than needing a flag.
inline constexpr cc::string_view snapshot_encoding_v1 = "snapshot-v1";

/// Why a snapshot's bytes would not decode.
/// String-free, exactly like op_decode_error and value_decode_error.
enum class snapshot_decode_error : u8
{
    truncated,
    unknown_encoding,
    index_out_of_range,
    unsorted_table,
    duplicate_table_entry,
    unsorted_entries,
    duplicate_entry,
    invalid_value,
    trailing_bytes,
};

/// Encodes a materialized document into `snapshot_encoding_v1`.
///
/// PRODUCER-SIDE ONLY: nothing on a load path may call this.
/// `doc` must be an UNFILTERED materialization of a single head, or what is stored is a projection rather than a
/// document, and every later sweep terminating on it is silently truncated.
[[nodiscard]] cc::vector<byte> encode_snapshot(vdoc::raw_document const& doc);

/// The only route from stored bytes to a snapshot, validating canonicality as it goes.
///
/// The result owns every byte it points at, so it outlives the ops that were pruned away.
/// The arena is built straight from `data` rather than through a second copy.
[[nodiscard]] cc::result<vdoc::snapshot_document, snapshot_decode_error> try_decode_snapshot(cc::string_view encoding,
                                                                                             cc::span<byte const> data);
} // namespace vdoc::file::impl
