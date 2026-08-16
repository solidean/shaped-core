#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <versioned-document-file/assets.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document-file/impl/rows.hh>

/// The two interfaces the shared loader and publisher speak, and the self-contained job that crosses to the actor thread.
///
/// Everything above this seam — decoding, verification, reachability, diagnostics — is written once and serves both
/// implementations, so the two arms cannot drift below the seam any more than they can at it.
/// Only reading rows out of storage and writing rows into it is implemented twice.
///
/// A failure here is always a HARD one: a reader that cannot produce its rows has nothing to hand the loader, and the
/// soft failures are decided a level up, from the rows themselves.

namespace vdoc::file::impl
{
/// Reads rows out of storage, in whatever order the loader asks for them.
///
/// Every method is a whole table, because history is small and loads eagerly and completely.
/// The one exception is `blobs`, whose payloads are never read here — read_chunk_summaries answers from row headers,
/// which is what keeps opening a multi-gigabyte file cheap.
class store_reader
{
public:
    virtual ~store_reader() = default;

    store_reader() = default;
    store_reader(store_reader const&) = delete;
    store_reader& operator=(store_reader const&) = delete;

    [[nodiscard]] virtual cc::result<cc::vector<blob_row>> read_blobs() = 0;
    [[nodiscard]] virtual cc::result<cc::vector<chunk_summary>> read_chunk_summaries() = 0;
    [[nodiscard]] virtual cc::result<cc::vector<asset_row>> read_assets() = 0;
    [[nodiscard]] virtual cc::result<cc::vector<op_row>> read_ops() = 0;
    [[nodiscard]] virtual cc::result<cc::vector<ref_row>> read_refs() = 0;
    [[nodiscard]] virtual cc::result<cc::vector<snapshot_row>> read_snapshots() = 0;

    /// Every chunk of one snapshot, in ascending chunk order.
    /// Asked per snapshot rather than as a whole table, because a snapshot is only ever wanted entire.
    [[nodiscard]] virtual cc::result<cc::vector<snapshot_chunk_row>> read_snapshot_chunks(cc::span<byte const> op_hash)
        = 0;
    [[nodiscard]] virtual cc::result<cc::vector<workspace_row>> read_workspace() = 0;
    [[nodiscard]] virtual cc::result<cc::vector<meta_row>> read_meta() = 0;

    /// Tables this build does not know, found while ensuring the schema.
    /// They are reported and then never touched again.
    [[nodiscard]] virtual cc::span<cc::string const> unknown_tables() const = 0;

    /// Extra columns on tables this build does know, as "<table>.<column>".
    [[nodiscard]] virtual cc::span<cc::string const> unknown_columns() const = 0;
};

/// Writes one publish, as one transaction.
///
/// The order the publisher calls these in is load-bearing: refs move LAST, so they are only visible once everything
/// they reach is in the same transaction.
class store_writer
{
public:
    virtual ~store_writer() = default;

    store_writer() = default;
    store_writer(store_writer const&) = delete;
    store_writer& operator=(store_writer const&) = delete;

    [[nodiscard]] virtual cc::result<cc::unit> begin() = 0;

    /// Content-addressed, so a conflict means the identical row is already there and nothing is rewritten.
    /// This is where idempotence comes from, rather than from a pre-check the caller could skip.
    [[nodiscard]] virtual cc::result<cc::unit> insert_op(op_row const& row) = 0;

    /// The rowid of a newly inserted blob, or empty when this hash was already stored.
    /// Empty means the caller writes no chunks: the bytes are already there.
    [[nodiscard]] virtual cc::result<cc::optional<i64>> insert_blob(blob_row const& row) = 0;
    [[nodiscard]] virtual cc::result<cc::unit> insert_chunk(chunk_row const& row) = 0;

    /// The one mutable mapping in the format, so this one really does overwrite.
    [[nodiscard]] virtual cc::result<cc::unit> upsert_asset(asset_row const& row) = 0;

    /// Unmaps a name.
    /// Removing an asset id that is not there is not an error — the outcome asked for already holds.
    [[nodiscard]] virtual cc::result<cc::unit> delete_asset(cc::string_view asset_id) = 0;

    /// Deletes a blob; its chunks follow by cascade, which is why foreign_keys must be on.
    [[nodiscard]] virtual cc::result<cc::unit> delete_blob(blob_hash const& hash) = 0;

    /// Attaches or replaces a snapshot and its chunks.
    ///
    /// Really overwrites, like upsert_asset and unlike insert_op: a snapshot is derived, and recomputing one is
    /// normal rather than a conflict.
    /// Any chunks the previous row had are gone afterwards, so a shorter payload cannot leave a longer one's tail.
    [[nodiscard]] virtual cc::result<cc::unit> upsert_snapshot(snapshot_row const& row,
                                                               cc::span<cc::vector<byte> const> chunks) = 0;

    /// Detaches a snapshot; its chunks follow by cascade, which is why foreign_keys must be on.
    /// Removing one that is not there is not an error — the outcome asked for already holds.
    [[nodiscard]] virtual cc::result<cc::unit> delete_snapshot(cc::span<byte const> op_hash) = 0;

    /// Empties an op's payload columns, leaving its id and its parents — a SKELETON.
    ///
    /// Deleting the row instead would sever ancestry through it, and two writes that were ordered would read as
    /// concurrent.
    [[nodiscard]] virtual cc::result<cc::unit> skeletonize_op(cc::span<byte const> op_hash) = 0;

    [[nodiscard]] virtual cc::result<cc::unit> upsert_ref(ref_row const& row) = 0;
    [[nodiscard]] virtual cc::result<cc::unit> upsert_workspace(workspace_row const& row) = 0;

    /// Publishes the transaction.
    /// Anything else — an error above, or this writer dying — rolls it back, so there is no observable half-publish.
    [[nodiscard]] virtual cc::result<cc::unit> commit() = 0;
};

/// A range of a blob's DECODED bytes.
/// A negative `size` means "to the end", which is what a whole-blob fetch asks for.
struct blob_fetch_range
{
    i64 offset = 0;
    i64 size = -1;
};

/// A blob's row facts, without a byte of its payload.
/// `id` is the storage-side row identity chunks are found by, and it never leaves the arm that produced it.
struct blob_header
{
    i64 id = 0;
    i64 decoded_size = 0;
    i64 stored_size = 0;
    i64 chunk_count = 0;
    cc::string encoding;
};

/// Reads blob payloads back out of storage, one blob at a time.
///
/// Separate from store_reader on purpose: a load reads no payload and a fetch reads nothing else, and the two run at
/// different times over different lifetimes — a reader is built once per open, this is built once per fetch.
class blob_payload_reader
{
public:
    virtual ~blob_payload_reader() = default;

    blob_payload_reader() = default;
    blob_payload_reader(blob_payload_reader const&) = delete;
    blob_payload_reader& operator=(blob_payload_reader const&) = delete;

    /// The row facts for `hash`, or empty where no blob has it.
    [[nodiscard]] virtual cc::result<cc::optional<blob_header>> read_blob_header(blob_hash const& hash) = 0;

    /// Reads exactly out.size() STORED bytes from `offset`, assembling across chunk boundaries.
    /// A range the chunks do not cover is an error — a torn blob is never a short fill.
    [[nodiscard]] virtual cc::result<cc::unit> read_stored_range(blob_header const& blob, i64 offset, cc::span<byte> out)
        = 0;
};

/// Reads one blob back and decodes it — the ONE route from a hash to bytes, shared by both implementations.
[[nodiscard]] cc::result<cc::vector<byte>> fetch_blob(blob_payload_reader& reader,
                                                      blob_hash const& hash,
                                                      blob_fetch_range range);

/// A blob and the bytes to store for it, as the publisher hands them to a writer.
/// `data` is empty when the upload said "you already have this", which the publisher has already validated.
struct blob_write
{
    blob_row row;
    cc::vector<byte> data;
    bool has_data = true;
};

/// One publish, computed on the calling thread and complete in itself.
///
/// Self-contained because it crosses to the actor thread: every byte here is a copy, so nothing it names can be
/// mutated or freed out from under the write.
/// It carries only the delta — the ops reachable from the refs that storage does not already have.
struct publish_job
{
    cc::vector<op_row> ops;
    cc::vector<blob_write> blobs;
    cc::vector<asset_row> assets;
    /// Names to unmap.
    /// Applied AFTER the upserts, so publishing an asset and removing it in one call removes it.
    /// Blobs the removed assets named are left alone: only a reclamation collects bytes.
    cc::vector<cc::string> removed_assets;
    /// Written LAST, which is what makes a ref move mean everything behind it landed.
    cc::vector<ref_row> refs;
};

/// One snapshot and the bytes to store for it, as the caller hands them to a writer.
struct snapshot_write
{
    snapshot_row row;
    cc::vector<cc::vector<byte>> chunks;
};

/// One snapshot write, with the ops it makes redundant — computed on the calling thread and complete in itself.
///
/// Self-contained for the same reason a publish_job is: it crosses to the actor thread, so every byte here is a copy.
/// The snapshots go in BEFORE anything is emptied, which is the order that makes a torn run impossible to get wrong
/// even though the transaction already makes it impossible to observe.
///
/// One job type serves both persisting a droppable snapshot and pruning, because the difference between them is
/// `required` plus whether `skeletonized` is empty — not a different write.
struct snapshot_write_job
{
    cc::vector<snapshot_write> snapshots;

    /// Op hashes to empty out, sorted by bytes so the write order is deterministic.
    cc::vector<cc::vector<byte>> skeletonized;
};

/// Runs one snapshot write over a writer, in one transaction.
[[nodiscard]] cc::result<snapshot_write_result> apply_snapshot_write(store_writer& writer, snapshot_write_job const& job);

/// One reclamation, computed on the calling thread from the resident asset index.
///
/// Both lists are already the answer: the closure walk happens above this, so the writer only deletes what it is told.
struct reclaim_job
{
    cc::vector<cc::string> removed_assets;
    cc::vector<blob_hash> removed_blobs;
};

/// Runs one reclamation over a writer, in one transaction.
[[nodiscard]] cc::result<reclaim_result> apply_reclaim(store_writer& writer, reclaim_job const& job);

/// How many bytes of a payload go in one chunk row, for blobs and snapshots alike.
///
/// Chunking sidesteps SQLite's per-value size ceiling and keeps a multi-gigabyte payload from being one allocation.
/// The value is a storage detail rather than a format constant: a reader is told the count, and never assumes it.
inline constexpr isize payload_chunk_size = 1 << 20;

/// Splits a payload into the chunks its table stores, in order.
///
/// Shared by blobs and snapshots so the two cannot end up chunking differently, which would only show up as a file
/// one build can read and another cannot.
/// An empty payload yields no chunks, and reassembling none of them is an empty payload again.
[[nodiscard]] cc::vector<cc::vector<byte>> split_into_chunks(cc::span<byte const> payload);

/// Runs one publish over a writer, in one transaction.
/// Shared by both store implementations, which is what keeps the write order — and its atomicity — identical on each.
[[nodiscard]] cc::result<publish_result> apply_publish(store_writer& writer, publish_job const& job);

/// Writes the dirty workspace entries, in one transaction.
/// Separate from apply_publish because a workspace failure is deliberately not latched.
[[nodiscard]] cc::result<cc::unit> apply_workspace(store_writer& writer, cc::span<workspace_entry const> entries);

/// Fills `target`'s loaded state and its report from `reader`.
/// Returns a hard failure, or nothing; every soft failure lands in the report and the load carries on.
[[nodiscard]] cc::result<cc::unit> load(store_reader& reader, store& target);
} // namespace vdoc::file::impl
