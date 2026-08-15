#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document-file/rows.hh>

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

    [[nodiscard]] virtual cc::result<cc::unit> upsert_ref(ref_row const& row) = 0;
    [[nodiscard]] virtual cc::result<cc::unit> upsert_workspace(workspace_row const& row) = 0;

    /// Publishes the transaction.
    /// Anything else — an error above, or this writer dying — rolls it back, so there is no observable half-publish.
    [[nodiscard]] virtual cc::result<cc::unit> commit() = 0;
};

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
    /// Written LAST, which is what makes a ref move mean everything behind it landed.
    cc::vector<ref_row> refs;
};

/// How many bytes of a blob go in one `blob_chunk` row.
///
/// Chunking sidesteps SQLite's per-value size ceiling and keeps a multi-gigabyte asset from being one allocation.
/// The value is a storage detail rather than a format constant: a reader is told the count, and never assumes it.
inline constexpr isize blob_chunk_size = 1 << 20;

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
