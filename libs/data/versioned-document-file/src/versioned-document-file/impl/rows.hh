#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <versioned-document-file/fwd.hh>

/// One struct per table, holding exactly the columns [the format](../../../docs/format.md#schema) names, in the column types it names.
///
/// **Untyped on purpose.** A hash is bytes rather than a blob_hash, and a payload is bytes rather than a decoded value,
/// because these are what a reader hands back *before* anything is verified — and a row that will not decode still has
/// to be reportable.
/// The loader is the one place those bytes become model types.
///
/// Both store implementations speak in these, which is what lets one loader and one publisher serve both.

/// A row of `ops`.
/// `metadata` and `assignments` are present together or absent together; absent is a skeleton op.
struct vdoc::file::op_row
{
    cc::vector<byte> hash;
    /// 32 bytes per parent, concatenated, in the op's own order.
    /// Carries no count: a length that is not a multiple of 32 is a decode error.
    cc::vector<byte> parents;
    cc::optional<cc::vector<byte>> metadata;
    cc::optional<cc::vector<byte>> assignments;
};

/// A row of `refs`.
struct vdoc::file::ref_row
{
    cc::string name;
    cc::vector<byte> op_hash;
};

/// A row of `snapshots`.
/// `required = 1` means history behind this op has been pruned, so deleting the row destroys data.
struct vdoc::file::snapshot_row
{
    cc::vector<byte> op_hash;
    i64 required = 0;
    cc::string encoding;
    cc::vector<byte> data;
};

/// A row of `assets`.
struct vdoc::file::asset_row
{
    cc::string asset_id;
    cc::string kind;
    /// An encoded vdoc value: an ordered array of part objects.
    cc::vector<byte> parts;
    cc::optional<cc::vector<byte>> meta;
    /// An encoded vdoc value: an array of asset id strings.
    /// Absent means no declared dependencies.
    cc::optional<cc::vector<byte>> deps;
};

/// A row of `blobs` — the metadata alone, never the payload.
/// `chunk_count` and `stored_size` say what must be there, so a blob whose chunks do not add up is visibly incomplete.
struct vdoc::file::blob_row
{
    i64 id = 0;
    cc::vector<byte> hash;
    i64 size = 0;
    i64 stored_size = 0;
    i64 chunk_count = 0;
    cc::string format;
    cc::string encoding;
};

/// A row of `blob_chunk`.
struct vdoc::file::chunk_row
{
    i64 blob_id = 0;
    i64 chunk_index = 0;
    cc::vector<byte> data;
};

/// What a load learns about one blob's chunks without reading a payload.
/// `total_bytes` comes from LENGTH(), which is answered from the row header.
struct vdoc::file::chunk_summary
{
    i64 blob_id = 0;
    i64 count = 0;
    i64 total_bytes = 0;
};

/// A row of `workspace`.
struct vdoc::file::workspace_row
{
    cc::string key;
    i64 version = 0;
    cc::vector<byte> value;
};

/// A row of `meta`.
struct vdoc::file::meta_row
{
    cc::string key;
    cc::optional<cc::vector<byte>> value;
};
