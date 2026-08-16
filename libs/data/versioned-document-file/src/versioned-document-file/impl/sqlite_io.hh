#pragma once

#include <babel-serializer/data/sqlite.hh>
#include <versioned-document-file/impl/sqlite_schema.hh>
#include <versioned-document-file/impl/store_io.hh>

/// The reader and writer over a live connection — the only place in this library that talks to a database.
///
/// Both are constructed on the actor thread, over the actor's own connection, and never leave it.

namespace vdoc::file::impl
{
/// Reads whole tables out of a connection.
///
/// Every statement NAMES ITS COLUMNS, and there is no `SELECT *` anywhere.
/// That is not a style preference: it is what makes a column a newer build added survive an open-modify-save cycle,
/// because nothing on the read or write path can address it.
class sqlite_reader final : public store_reader
{
public:
    sqlite_reader(babel::sqlite::database& db, schema_scan scan) : _db(db), _scan(cc::move(scan)) {}

    [[nodiscard]] cc::result<cc::vector<blob_row>> read_blobs() override;
    [[nodiscard]] cc::result<cc::vector<chunk_summary>> read_chunk_summaries() override;
    [[nodiscard]] cc::result<cc::vector<asset_row>> read_assets() override;
    [[nodiscard]] cc::result<cc::vector<op_row>> read_ops() override;
    [[nodiscard]] cc::result<cc::vector<ref_row>> read_refs() override;
    [[nodiscard]] cc::result<cc::vector<snapshot_row>> read_snapshots() override;
    [[nodiscard]] cc::result<cc::vector<snapshot_chunk_row>> read_snapshot_chunks(cc::span<byte const> op_hash) override;
    [[nodiscard]] cc::result<cc::vector<workspace_row>> read_workspace() override;
    [[nodiscard]] cc::result<cc::vector<meta_row>> read_meta() override;

    [[nodiscard]] cc::span<cc::string const> unknown_tables() const override { return _scan.unknown_tables; }
    [[nodiscard]] cc::span<cc::string const> unknown_columns() const override { return _scan.unknown_columns; }

private:
    babel::sqlite::database& _db;
    schema_scan _scan;
};

/// Reads blob payloads back out of a connection, through incremental blob I/O.
///
/// Chunk rowids are resolved PER FETCH rather than cached at load, and that is a correctness choice rather than a
/// laziness: a publish after open inserts chunks the load never scanned, and a VACUUM renumbers rowids under a file
/// another process holds — a stale cache would hand back wrong bytes rather than merely be slow.
/// One indexed lookup in front of a payload read costs microseconds against milliseconds.
class sqlite_blob_payload_reader final : public blob_payload_reader
{
public:
    explicit sqlite_blob_payload_reader(babel::sqlite::database& db) : _db(db) {}

    [[nodiscard]] cc::result<cc::optional<blob_header>> read_blob_header(blob_hash const& hash) override;
    [[nodiscard]] cc::result<cc::unit> read_stored_range(blob_header const& blob, i64 offset, cc::span<byte> out) override;

private:
    babel::sqlite::database& _db;
};

/// Writes one publish into a connection, as one transaction.
///
/// The transaction lives here rather than in the caller, so that this writer dying — for any reason, on any path — rolls it back.
/// There is no route to an observable half-publish.
class sqlite_writer final : public store_writer
{
public:
    explicit sqlite_writer(babel::sqlite::database& db) : _db(db) {}

    [[nodiscard]] cc::result<cc::unit> begin() override;
    [[nodiscard]] cc::result<cc::unit> insert_op(op_row const& row) override;
    [[nodiscard]] cc::result<cc::optional<i64>> insert_blob(blob_row const& row) override;
    [[nodiscard]] cc::result<cc::unit> insert_chunk(chunk_row const& row) override;
    [[nodiscard]] cc::result<cc::unit> upsert_asset(asset_row const& row) override;
    [[nodiscard]] cc::result<cc::unit> delete_asset(cc::string_view asset_id) override;
    [[nodiscard]] cc::result<cc::unit> delete_blob(blob_hash const& hash) override;
    [[nodiscard]] cc::result<cc::unit> upsert_snapshot(snapshot_row const& row,
                                                       cc::span<cc::vector<byte> const> chunks) override;
    [[nodiscard]] cc::result<cc::unit> delete_snapshot(cc::span<byte const> op_hash) override;
    [[nodiscard]] cc::result<cc::unit> skeletonize_op(cc::span<byte const> op_hash) override;
    [[nodiscard]] cc::result<cc::unit> upsert_ref(ref_row const& row) override;
    [[nodiscard]] cc::result<cc::unit> upsert_workspace(workspace_row const& row) override;
    [[nodiscard]] cc::result<cc::unit> commit() override;

private:
    babel::sqlite::database& _db;
    cc::optional<babel::sqlite::transaction> _transaction;
};
} // namespace vdoc::file::impl
