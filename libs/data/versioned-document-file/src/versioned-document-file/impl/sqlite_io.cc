#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/sqlite_io.hh>

namespace vdoc::file::impl
{
namespace
{
namespace sql = babel::sqlite;

cc::vector<byte> copy_blob(sql::row const& row, i32 column)
{
    return cc::vector<byte>::create_copy_of(row.as_blob(column));
}

cc::optional<cc::vector<byte>> copy_nullable_blob(sql::row const& row, i32 column)
{
    if (row.is_null(column))
        return {};
    return copy_blob(row, column);
}

/// Binds an optional blob column, as the blob or as NULL.
/// Absent and empty are different here: an empty vdoc value still decodes, and NULL is what "no column" means.
cc::result<cc::unit> bind_nullable_blob(sql::statement& stmt, i32 index, cc::optional<cc::vector<byte>> const& value)
{
    if (value.has_value())
        return stmt.bind(index, cc::span<byte const>(value.value()));
    return stmt.bind_null(index);
}

/// Runs one statement to completion, ignoring its result rows.
cc::result<cc::unit> step_to_done(sql::statement& stmt)
{
    while (true)
    {
        auto const stepped = stmt.next();
        CC_RETURN_IF_ERROR(stepped);
        if (!stepped.value())
            return cc::unit{};
    }
}

/// Reads every row of `query` through `read`, turning the statement's sticky failure back into a result.
template <class RowT, class ReadT>
cc::result<cc::vector<RowT>> read_all(sql::database& db, cc::string_view query, ReadT&& read)
{
    auto stmt = db.query(query);
    CC_RETURN_IF_ERROR(stmt);

    auto out = cc::vector<RowT>();
    for (auto const row : stmt.value())
        out.push_back(read(row));

    // A step failure ends a range-for silently, so it is picked up here rather than mistaken for an empty table.
    if (!stmt.value().is_ok())
        return cc::error(cc::any_error(cc::string(stmt.value().last_error().message)));
    return out;
}
} // namespace

// sqlite_reader
// -------------------------------------------------------------------------------------------------

cc::result<cc::vector<blob_row>> sqlite_reader::read_blobs()
{
    return read_all<blob_row>(_db, "SELECT id, hash, size, stored_size, chunk_count, format, encoding FROM blobs",
                              [](sql::row const& row)
                              {
                                  return blob_row{.id = row.as_i64(0),
                                                  .hash = copy_blob(row, 1),
                                                  .size = row.as_i64(2),
                                                  .stored_size = row.as_i64(3),
                                                  .chunk_count = row.as_i64(4),
                                                  .format = cc::string(row.as_string(5)),
                                                  .encoding = cc::string(row.as_string(6))};
                              });
}

cc::result<cc::vector<chunk_summary>> sqlite_reader::read_chunk_summaries()
{
    // LENGTH() is answered from the row header, and no payload is ever read here.
    // That is what keeps opening a multi-gigabyte file cheap.
    return read_all<chunk_summary>(
        _db, "SELECT blob_id, COUNT(*), SUM(LENGTH(data)) FROM blob_chunk GROUP BY blob_id", [](sql::row const& row)
        { return chunk_summary{.blob_id = row.as_i64(0), .count = row.as_i64(1), .total_bytes = row.as_i64(2)}; });
}

cc::result<cc::vector<asset_row>> sqlite_reader::read_assets()
{
    return read_all<asset_row>(_db, "SELECT asset_id, kind, parts, meta, deps FROM assets",
                               [](sql::row const& row)
                               {
                                   return asset_row{.asset_id = cc::string(row.as_string(0)),
                                                    .kind = cc::string(row.as_string(1)),
                                                    .parts = copy_blob(row, 2),
                                                    .meta = copy_nullable_blob(row, 3),
                                                    .deps = copy_nullable_blob(row, 4)};
                               });
}

cc::result<cc::vector<op_row>> sqlite_reader::read_ops()
{
    return read_all<op_row>(_db, "SELECT hash, parents, metadata, assignments FROM ops",
                            [](sql::row const& row)
                            {
                                return op_row{.hash = copy_blob(row, 0),
                                              .parents = copy_blob(row, 1),
                                              .metadata = copy_nullable_blob(row, 2),
                                              .assignments = copy_nullable_blob(row, 3)};
                            });
}

cc::result<cc::vector<ref_row>> sqlite_reader::read_refs()
{
    return read_all<ref_row>(_db, "SELECT name, op_hash FROM refs", [](sql::row const& row)
                             { return ref_row{.name = cc::string(row.as_string(0)), .op_hash = copy_blob(row, 1)}; });
}

cc::result<cc::vector<snapshot_row>> sqlite_reader::read_snapshots()
{
    return read_all<snapshot_row>(
        _db, "SELECT op_hash, required, encoding, decoded_size, stored_size, chunk_count FROM snapshots",
        [](sql::row const& row)
        {
            return snapshot_row{.op_hash = copy_blob(row, 0),
                                .required = row.as_i64(1),
                                .encoding = cc::string(row.as_string(2)),
                                .decoded_size = row.as_i64(3),
                                .stored_size = row.as_i64(4),
                                .chunk_count = row.as_i64(5)};
        });
}

cc::result<cc::vector<snapshot_chunk_row>> sqlite_reader::read_snapshot_chunks(cc::span<byte const> op_hash)
{
    auto stmt = _db.prepare("SELECT chunk_index, data FROM snapshot_chunk WHERE op_hash = ?1 ORDER BY chunk_index");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, op_hash));

    auto out = cc::vector<snapshot_chunk_row>();
    while (true)
    {
        auto const stepped = stmt.value().next();
        CC_RETURN_IF_ERROR(stepped);
        if (!stepped.value())
            return out;

        auto const row = stmt.value().current();
        out.push_back({.op_hash = cc::vector<byte>::create_copy_of(op_hash),
                       .chunk_index = row.as_i64(0),
                       .data = copy_blob(row, 1)});
    }
}

cc::result<cc::vector<workspace_row>> sqlite_reader::read_workspace()
{
    return read_all<workspace_row>(_db, "SELECT key, version, value FROM workspace",
                                   [](sql::row const& row)
                                   {
                                       return workspace_row{.key = cc::string(row.as_string(0)),
                                                            .version = row.as_i64(1),
                                                            .value = copy_blob(row, 2)};
                                   });
}

cc::result<cc::vector<meta_row>> sqlite_reader::read_meta()
{
    return read_all<meta_row>(
        _db, "SELECT key, value FROM meta", [](sql::row const& row)
        { return meta_row{.key = cc::string(row.as_string(0)), .value = copy_nullable_blob(row, 1)}; });
}

// sqlite_blob_payload_reader
// -------------------------------------------------------------------------------------------------

cc::result<cc::optional<blob_header>> sqlite_blob_payload_reader::read_blob_header(blob_hash const& hash)
{
    byte hash_bytes[blob_hash::byte_size] = {};
    hash.to_bytes(hash_bytes);

    auto stmt = _db.prepare("SELECT id, size, stored_size, chunk_count, encoding FROM blobs WHERE hash = ?1");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::span<byte const>(hash_bytes)));

    auto const stepped = stmt.value().next();
    CC_RETURN_IF_ERROR(stepped);
    if (!stepped.value())
        return cc::optional<blob_header>();

    auto const row = stmt.value().current();
    return cc::optional<blob_header>(blob_header{.id = row.as_i64(0),
                                                 .decoded_size = row.as_i64(1),
                                                 .stored_size = row.as_i64(2),
                                                 .chunk_count = row.as_i64(3),
                                                 .encoding = cc::string(row.as_string(4))});
}

cc::result<cc::unit> sqlite_blob_payload_reader::read_stored_range(blob_header const& blob, i64 offset, cc::span<byte> out)
{
    if (out.empty())
        return cc::unit{};

    struct chunk_extent
    {
        i64 rowid = 0;
        i64 size = 0;
    };

    // LENGTH() is answered from the row header, so this walk costs an index scan and reads no payload.
    auto extents = cc::vector<chunk_extent>();
    {
        auto stmt = _db.prepare("SELECT id, LENGTH(data) FROM blob_chunk WHERE blob_id = ?1 ORDER BY chunk_index");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, blob.id));

        for (auto const row : stmt.value())
            extents.push_back({.rowid = row.as_i64(0), .size = row.as_i64(1)});
        if (!stmt.value().is_ok())
            return cc::error(cc::any_error(cc::string(stmt.value().last_error().message)));
    }

    // A handle is only opened once the walk reaches the first overlapping chunk, and then reopened along it.
    // Nothing here can be invalidated by this store's own writes: a publish and a fetch are two messages on one actor,
    // which dispatches them one at a time.
    // A FOREIGN writer still can, and read_at reports that rather than handing back stale bytes — which is why there
    // is no retry.
    auto handle = cc::optional<babel::sqlite::blob_handle>();
    auto at = i64(0); // where this chunk starts, in the blob's stored bytes
    auto filled = i64(0);
    for (auto const& extent : extents)
    {
        if (filled == out.size())
            break;
        auto const chunk_end = at + extent.size;
        if (chunk_end <= offset)
        {
            at = chunk_end;
            continue;
        }

        auto const from = cc::max(i64(0), offset - at);
        auto const take = cc::min(extent.size - from, i64(out.size()) - filled);

        if (!handle.has_value())
        {
            auto opened = _db.open_blob_handle({.table = "blob_chunk", .column = "data", .rowid = extent.rowid});
            CC_RETURN_IF_ERROR(opened);
            handle = cc::move(opened.value());
        }
        else
        {
            CC_RETURN_IF_ERROR(handle.value().reopen(extent.rowid));
        }

        CC_RETURN_IF_ERROR(handle.value().read_at(from, out.subspan({.offset = filled, .size = take})));
        filled += take;
        at = chunk_end;
    }

    // A short walk means the chunks do not cover the range — a torn blob, reported rather than silently zero-filled.
    if (filled != out.size())
        return cc::error(
            cc::any_error(cc::format("a blob's chunks cover {} of the {} bytes asked for", filled, out.size())));
    return cc::unit{};
}

// sqlite_writer
// -------------------------------------------------------------------------------------------------

cc::result<cc::unit> sqlite_writer::begin()
{
    auto opened = _db.begin_transaction();
    CC_RETURN_IF_ERROR(opened);
    _transaction = cc::move(opened.value());
    return cc::unit{};
}

cc::result<cc::unit> sqlite_writer::insert_op(op_row const& row)
{
    // ON CONFLICT DO NOTHING is where idempotence comes from: the key IS the content hash, so a conflict means the
    // identical row is already there.
    auto stmt = _db.prepare("INSERT INTO ops(hash, parents, metadata, assignments) VALUES (?1, ?2, ?3, ?4)"
                            " ON CONFLICT(hash) DO NOTHING");
    CC_RETURN_IF_ERROR(stmt);

    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::span<byte const>(row.hash)));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, cc::span<byte const>(row.parents)));
    // Braced: CC_RETURN_IF_ERROR expands to an `if`, so an unbraced `else` would bind to ITS `if` and run both arms.
    if (row.metadata.has_value())
    {
        CC_RETURN_IF_ERROR(stmt.value().bind(3, cc::span<byte const>(row.metadata.value())));
    }
    else
    {
        CC_RETURN_IF_ERROR(stmt.value().bind_null(3));
    }
    if (row.assignments.has_value())
    {
        CC_RETURN_IF_ERROR(stmt.value().bind(4, cc::span<byte const>(row.assignments.value())));
    }
    else
    {
        CC_RETURN_IF_ERROR(stmt.value().bind_null(4));
    }

    return step_to_done(stmt.value());
}

cc::result<cc::optional<i64>> sqlite_writer::insert_blob(blob_row const& row)
{
    auto stmt = _db.prepare("INSERT INTO blobs(hash, size, stored_size, chunk_count, format, encoding)"
                            " VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT(hash) DO NOTHING");
    CC_RETURN_IF_ERROR(stmt);

    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::span<byte const>(row.hash)));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, row.size));
    CC_RETURN_IF_ERROR(stmt.value().bind(3, row.stored_size));
    CC_RETURN_IF_ERROR(stmt.value().bind(4, row.chunk_count));
    CC_RETURN_IF_ERROR(stmt.value().bind(5, cc::string_view(row.format)));
    CC_RETURN_IF_ERROR(stmt.value().bind(6, cc::string_view(row.encoding)));
    CC_RETURN_IF_ERROR(step_to_done(stmt.value()));

    // No row changed means the hash was already stored, so its bytes are already there and no chunk is written.
    if (_db.changes() == 0)
        return cc::optional<i64>();
    return cc::optional<i64>(_db.last_insert_rowid());
}

cc::result<cc::unit> sqlite_writer::insert_chunk(chunk_row const& row)
{
    auto stmt = _db.prepare("INSERT INTO blob_chunk(blob_id, chunk_index, data) VALUES (?1, ?2, ?3)");
    CC_RETURN_IF_ERROR(stmt);

    CC_RETURN_IF_ERROR(stmt.value().bind(1, row.blob_id));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, row.chunk_index));
    CC_RETURN_IF_ERROR(stmt.value().bind(3, cc::span<byte const>(row.data)));
    return step_to_done(stmt.value());
}

cc::result<cc::unit> sqlite_writer::upsert_asset(asset_row const& row)
{
    // The one mutable mapping in the format, so this one really does overwrite — and it names only the columns this
    // build owns, which is what leaves a newer build's column alone.
    auto stmt = _db.prepare("INSERT INTO assets(asset_id, kind, parts, meta, deps) VALUES (?1, ?2, ?3, ?4, ?5)"
                            " ON CONFLICT(asset_id) DO UPDATE SET kind = excluded.kind, parts = excluded.parts,"
                            " meta = excluded.meta, deps = excluded.deps");
    CC_RETURN_IF_ERROR(stmt);

    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(row.asset_id)));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, cc::string_view(row.kind)));
    CC_RETURN_IF_ERROR(stmt.value().bind(3, cc::span<byte const>(row.parts)));
    CC_RETURN_IF_ERROR(bind_nullable_blob(stmt.value(), 4, row.meta));
    CC_RETURN_IF_ERROR(bind_nullable_blob(stmt.value(), 5, row.deps));

    return step_to_done(stmt.value());
}

cc::result<cc::unit> sqlite_writer::delete_asset(cc::string_view asset_id)
{
    auto stmt = _db.prepare("DELETE FROM assets WHERE asset_id = ?1");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, asset_id));
    return step_to_done(stmt.value());
}

cc::result<cc::unit> sqlite_writer::delete_blob(blob_hash const& hash)
{
    byte hash_bytes[blob_hash::byte_size] = {};
    hash.to_bytes(hash_bytes);

    // blob_chunk follows by ON DELETE CASCADE, which only fires because ensure_schema turns foreign_keys on and reads
    // the pragma back rather than assuming the request took.
    auto stmt = _db.prepare("DELETE FROM blobs WHERE hash = ?1");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::span<byte const>(hash_bytes)));
    return step_to_done(stmt.value());
}

cc::result<cc::unit> sqlite_writer::upsert_snapshot(snapshot_row const& row, cc::span<cc::vector<byte> const> chunks)
{
    // The old chunks go first: a replacement payload that is shorter would otherwise leave the tail of the longer one
    // behind, and chunk_count would disagree with what is actually stored.
    auto cleared = _db.prepare("DELETE FROM snapshot_chunk WHERE op_hash = ?1");
    CC_RETURN_IF_ERROR(cleared);
    CC_RETURN_IF_ERROR(cleared.value().bind(1, cc::span<byte const>(row.op_hash)));
    CC_RETURN_IF_ERROR(step_to_done(cleared.value()));

    auto stmt = _db.prepare("INSERT INTO snapshots(op_hash, required, encoding, decoded_size, stored_size, chunk_count)"
                            " VALUES (?1, ?2, ?3, ?4, ?5, ?6)"
                            " ON CONFLICT(op_hash) DO UPDATE SET required = excluded.required,"
                            " encoding = excluded.encoding, decoded_size = excluded.decoded_size,"
                            " stored_size = excluded.stored_size, chunk_count = excluded.chunk_count");
    CC_RETURN_IF_ERROR(stmt);

    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::span<byte const>(row.op_hash)));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, row.required));
    CC_RETURN_IF_ERROR(stmt.value().bind(3, cc::string_view(row.encoding)));
    CC_RETURN_IF_ERROR(stmt.value().bind(4, row.decoded_size));
    CC_RETURN_IF_ERROR(stmt.value().bind(5, row.stored_size));
    CC_RETURN_IF_ERROR(stmt.value().bind(6, row.chunk_count));
    CC_RETURN_IF_ERROR(step_to_done(stmt.value()));

    for (isize i = 0; i < chunks.size(); ++i)
    {
        auto chunk = _db.prepare("INSERT INTO snapshot_chunk(op_hash, chunk_index, data) VALUES (?1, ?2, ?3)");
        CC_RETURN_IF_ERROR(chunk);
        CC_RETURN_IF_ERROR(chunk.value().bind(1, cc::span<byte const>(row.op_hash)));
        CC_RETURN_IF_ERROR(chunk.value().bind(2, i64(i)));
        CC_RETURN_IF_ERROR(chunk.value().bind(3, cc::span<byte const>(chunks[i])));
        CC_RETURN_IF_ERROR(step_to_done(chunk.value()));
    }

    return cc::unit{};
}

cc::result<cc::unit> sqlite_writer::delete_snapshot(cc::span<byte const> op_hash)
{
    auto stmt = _db.prepare("DELETE FROM snapshots WHERE op_hash = ?1");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, op_hash));
    return step_to_done(stmt.value());
}

cc::result<cc::unit> sqlite_writer::skeletonize_op(cc::span<byte const> op_hash)
{
    // The row stays and keeps its parents, so ancestry through it survives; only the payload goes.
    auto stmt = _db.prepare("UPDATE ops SET metadata = NULL, assignments = NULL WHERE hash = ?1");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, op_hash));
    return step_to_done(stmt.value());
}

cc::result<cc::unit> sqlite_writer::upsert_ref(ref_row const& row)
{
    auto stmt = _db.prepare("INSERT INTO refs(name, op_hash) VALUES (?1, ?2)"
                            " ON CONFLICT(name) DO UPDATE SET op_hash = excluded.op_hash");
    CC_RETURN_IF_ERROR(stmt);

    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(row.name)));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, cc::span<byte const>(row.op_hash)));
    return step_to_done(stmt.value());
}

cc::result<cc::unit> sqlite_writer::upsert_workspace(workspace_row const& row)
{
    // One statement per DIRTY key, so a key a newer build wrote and this one never touched is not addressed at all.
    auto stmt = _db.prepare("INSERT INTO workspace(key, version, value) VALUES (?1, ?2, ?3)"
                            " ON CONFLICT(key) DO UPDATE SET version = excluded.version, value = excluded.value");
    CC_RETURN_IF_ERROR(stmt);

    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(row.key)));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, row.version));
    CC_RETURN_IF_ERROR(stmt.value().bind(3, cc::span<byte const>(row.value)));
    return step_to_done(stmt.value());
}

cc::result<cc::unit> sqlite_writer::commit()
{
    if (!_transaction.has_value())
        return cc::error(cc::any_error(cc::string("committing a transaction that was never begun")));

    auto committed = _transaction.value().commit();
    CC_RETURN_IF_ERROR(committed);
    _transaction = {};
    return cc::unit{};
}
} // namespace vdoc::file::impl
