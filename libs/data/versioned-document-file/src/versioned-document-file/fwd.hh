#pragma once

#include <clean-core/fwd.hh>
#include <versioned-document/fwd.hh>

/// Aggregate forward declarations for versioned-document-file.
///
/// This header is also the API index: every name the library exposes is declared here, with the one line that says what it is.
/// The on-disk shape is specified by [the format](../../docs/format.md).
/// The model it stores is [versioned-document](../../../versioned-document/docs/_index.md#concepts)'s.
///
/// A `.vdoc` file holds three kinds of state, and only the first is immutable:
///   the op DAG plus its refs and snapshots — content-addressed, verified on load, append-only
///   the asset index over deduplicated blobs — blobs immutable, the name -> asset mapping deliberately not
///   the workspace — disposable, discardable in its entirety without touching the document

namespace vdoc::file
{
// Pull in the shaped-core vocabulary types (i32, u8, isize, ...) so we write them bare inside vdoc::file
// without leaking them into the global namespace.
using namespace cc::primitive_defines;
} // namespace vdoc::file

// ---- the store ---------------------------------------------------------------------------------

namespace vdoc::file
{
/// One `.vdoc` file: the op DAG, the asset index, the workspace, and the ability to publish changes back.
///
/// The interface is the seam.
/// A SQLite-backed store and an in-memory one satisfy it, and one conformance suite runs against both.
class store;

/// Fetches asset blob bytes on demand.
/// Handed to whatever resolves assets for the application; the store itself never interprets a blob.
class blob_source;

/// What a caller asks to publish: ref moves plus assets and blobs.
/// Ops are not listed — the store derives them from the refs by reachability, so an op no ref can reach cannot be published by mistake.
struct publish_changes;

/// What a publish actually had to write.
/// Both counts are zero for a publish that was already durable, which is what idempotence looks like from the outside.
struct publish_result;

/// What a reclamation actually collected.
struct reclaim_result;

/// What a prune actually emptied.
struct snapshot_write_result;

/// What recovering history from a peer filled in, and which required snapshots it made droppable.
struct recovery_result;

/// An asset id resolved to its record plus a source to fetch its parts through.
struct asset_resolution;

/// The two halves of an open: the store, and when its load finished.
/// Separate because the CALLER must own the store from the first instant — an actor that held the last reference would tear itself down.
struct open_result;
} // namespace vdoc::file

// ---- load diagnostics --------------------------------------------------------------------------
//
// Soft failures, string-free, mirroring the model library's diagnostics.
// None of these block a load: the damaged part is dropped and reported, and the rest of the file opens.

namespace vdoc::file
{
/// Why part of a file would not load.
enum class load_issue_kind : u8;

/// One thing that would not load, and which op or asset it concerns.
struct load_issue;

/// Everything a load found.
struct load_report;
} // namespace vdoc::file

// ---- assets and blobs --------------------------------------------------------------------------
//
// An asset is a name pointing at a list of parts; a part points at a blob.
// Blobs are content-addressed and shared, so two assets that happen to hold the same bytes store them once.

namespace vdoc::file
{
/// Content hash of a blob: a 32-byte BLAKE3 digest of the decoded bytes.
struct blob_hash;

/// One part of an asset.
/// Addressed by (name, index); the name is the contract, and the index disambiguates parts sharing one.
struct asset_part;

/// Why a part lookup did not produce exactly one part.
enum class part_lookup_error : u8;

/// The parts of one asset sharing a name, in declaration order.
class part_range;

/// One row of the asset index: what the asset is, its parts, and its informational metadata.
struct asset_record;

/// A blob offered for storage, in whatever encoding it is stored as.
struct blob_upload;
} // namespace vdoc::file

// ---- the storage rows ----------------------------------------------------------------------------
//
// One struct per table, holding exactly the columns the format names, in the column types it names.
// Untyped on purpose: this is what a reader hands back BEFORE anything is verified, and a row that will not decode still has to be reportable.

namespace vdoc::file
{
struct op_row;
struct ref_row;
struct snapshot_row;
struct snapshot_chunk_row;
struct asset_row;
struct blob_row;
struct chunk_row;

/// What a load learns about one blob's chunks without ever reading a payload.
struct chunk_summary;

struct workspace_row;
struct meta_row;
} // namespace vdoc::file

// ---- the in-memory backing ----------------------------------------------------------------------

namespace vdoc::file
{
/// The whole of an in-memory store's state, in the same rows a file holds.
///
/// It outlives the store that wrote it, which is what lets a caller — and the conformance suite — close and reopen one exactly like a file.
struct memory_image;
} // namespace vdoc::file

// ---- snapshots and workspace ---------------------------------------------------------------------

namespace vdoc::file
{
/// A materialized document cached against an op, so loading need not replay history back to the root.
struct snapshot_entry;

/// One workspace value plus the version that describes its shape.
/// A reader that does not know the version skips the entry rather than dropping it.
struct workspace_value;

/// One keyed workspace entry, as written to and read from the file.
struct workspace_entry;
} // namespace vdoc::file
