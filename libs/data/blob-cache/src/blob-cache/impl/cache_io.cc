#include <blob-cache/impl/cache_io.hh>
#include <blob-cache/impl/cache_schema.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>

namespace bcache::impl
{
namespace
{
namespace sql = babel::sqlite;

cc::any_error as_error(sql::error const& e)
{
    return cc::any_error(cc::string(e.message));
}

/// A statement's own sticky failure, which a range-for silently ends on rather than reporting.
cc::result<cc::unit> check(sql::statement const& stmt)
{
    if (!stmt.is_ok())
        return cc::error(as_error(stmt.last_error()));
    return cc::unit{};
}

cc::vector<byte> hash_bytes(content_hash const& h)
{
    auto bytes = cc::vector<byte>::create_uninitialized(32);
    h.value.to_bytes(bytes);
    return bytes;
}

content_hash hash_from(cc::span<byte const> bytes)
{
    if (bytes.size() != 32)
        return {}; // a row this build wrote cannot look like this; a corrupt one reads as a hash nothing matches
    return {cc::hash256::from_bytes(bytes)};
}
} // namespace

// ---- reading -------------------------------------------------------------------------------------

cc::result<cc::optional<entry_row>> cache_reader::find_entry(cache_key const& key)
{
    auto stmt = _db.query("SELECT e.id, e.object_id, e.expires_at, e.metadata, o.hash, o.size, o.chunk_count"
                          " FROM entries e JOIN objects o ON o.id = e.object_id"
                          " WHERE e.namespace = ?1 AND e.key = ?2 AND e.version = ?3");
    CC_RETURN_IF_ERROR(stmt);

    CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(key.space.name)));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, cc::span<byte const>(key.key.bytes)));
    CC_RETURN_IF_ERROR(stmt.value().bind(3, i64(key.version)));

    auto const has_row = stmt.value().next();
    CC_RETURN_IF_ERROR(has_row);
    if (!has_row.value())
        return cc::optional<entry_row>();

    auto const row = stmt.value().current();
    auto out = entry_row{.entry_id = row.as_i64(0),
                         .object_id = row.as_i64(1),
                         .hash = hash_from(row.as_blob(4)),
                         .size = row.as_i64(5),
                         .chunk_count = row.as_i64(6),
                         .expires_at = row.as_double(2)};
    if (!row.is_null(3))
        out.metadata = cc::vector<byte>::create_copy_of(row.as_blob(3));

    return cc::optional<entry_row>(cc::move(out));
}

cc::result<blob> cache_reader::read_object(entry_row const& entry)
{
    // One allocation for the whole object, filled in place.
    // The pin is what the caller ends up owning, and nothing here — not the cache, not the actor, not SQLite — retains a reference to it afterwards.
    auto out = cc::pinned_data<byte>::create_uninitialized(entry.size);

    if (entry.size == 0)
        return blob(out);

    // Rowids are resolved PER READ rather than cached, and that is a correctness choice rather than laziness:
    // a VACUUM renumbers rowids under a file another process holds, and a stale cache would hand back wrong bytes.
    auto stmt = _db.query("SELECT id FROM object_chunk WHERE object_id = ?1 ORDER BY chunk_index");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, entry.object_id));

    auto rowids = cc::vector<i64>();
    for (auto const row : stmt.value())
        rowids.push_back(row.as_i64(0));
    CC_RETURN_IF_ERROR(check(stmt.value()));

    if (rowids.size() != entry.chunk_count)
        return cc::error(cc::any_error(
            cc::format("object {} claims {} chunks but has {}", entry.object_id, entry.chunk_count, rowids.size())));

    auto handle = _db.open_blob_handle({.table = "object_chunk", .column = "data", .rowid = rowids.front()});
    CC_RETURN_IF_ERROR(handle);

    auto offset = i64(0);
    for (auto i = isize(0); i < rowids.size(); ++i)
    {
        if (i != 0)
            CC_RETURN_IF_ERROR(handle.value().reopen(rowids[i]));

        auto const n = handle.value().size();
        if (offset + n > entry.size)
            return cc::error(cc::any_error(cc::format("object {} is longer than its recorded size", entry.object_id)));

        CC_RETURN_IF_ERROR(handle.value().read_at(0, out.span().subspan({.offset = offset, .size = n})));
        offset += n;
    }

    if (offset != entry.size)
        return cc::error(cc::any_error(cc::format("object {} is shorter than its recorded size", entry.object_id)));

    return blob(out);
}

cc::result<size_totals> cache_reader::read_totals()
{
    auto out = size_totals();

    auto stmt = _db.query("SELECT (SELECT COALESCE(SUM(size), 0) FROM objects), (SELECT COUNT(*) FROM entries)");
    CC_RETURN_IF_ERROR(stmt);
    auto const has_row = stmt.value().next();
    CC_RETURN_IF_ERROR(has_row);
    if (has_row.value())
    {
        out.stored_bytes = stmt.value().current().as_i64(0);
        out.entry_count = stmt.value().current().as_i64(1);
    }

    // The disk figure, which the object sum deliberately is not: pages, indexes, the freelist and the WAL are all in here and none of them are in that.
    auto pages = _db.query("PRAGMA page_count");
    auto page_size = _db.query("PRAGMA page_size");
    if (pages.has_value() && page_size.has_value())
    {
        auto const a = pages.value().next();
        auto const b = page_size.value().next();
        if (a.has_value() && a.value() && b.has_value() && b.value())
            out.file_bytes = pages.value().current().as_i64(0) * page_size.value().current().as_i64(0);
    }

    return out;
}

// ---- writing -------------------------------------------------------------------------------------

cc::result<put_status> cache_writer::insert(put_row const& row, blob const& data)
{
    auto const hash = hash_bytes(row.hash);

    // Spelled out rather than int_div_round_up, which requires a positive numerator: an EMPTY object is a legitimate
    // cached value with zero chunks, and it must round to 0 rather than assert.
    auto const chunk_count = row.size == 0 ? i64(0) : 1 + (row.size - 1) / chunk_size_bytes;

    auto transaction = _db.begin_transaction();
    CC_RETURN_IF_ERROR(transaction);

    // The object first.
    // INSERT OR IGNORE is the whole concurrency story: whoever loses sees changes() == 0 and attaches to the winner's row, which it can only observe once that writer committed.
    {
        auto stmt = _db.query("INSERT OR IGNORE INTO objects(hash, size, chunk_count, created_at, refcount)"
                              " VALUES(?1, ?2, ?3, ?4, 0)");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::span<byte const>(hash)));
        CC_RETURN_IF_ERROR(stmt.value().bind(2, row.size));
        CC_RETURN_IF_ERROR(stmt.value().bind(3, chunk_count));
        CC_RETURN_IF_ERROR(stmt.value().bind(4, row.created_at));
        CC_RETURN_IF_ERROR(stmt.value().next());
        CC_RETURN_IF_ERROR(check(stmt.value()));
    }
    auto const created_object = _db.changes() == 1;

    // Never last_insert_rowid(): it is meaningless after an IGNORE that ignored.
    auto object_id = i64(0);
    {
        auto stmt = _db.query("SELECT id FROM objects WHERE hash = ?1");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::span<byte const>(hash)));
        auto const has_row = stmt.value().next();
        CC_RETURN_IF_ERROR(has_row);
        if (!has_row.value())
            return cc::error(cc::any_error(cc::string("the object row vanished inside our own transaction")));
        object_id = stmt.value().current().as_i64(0);
    }

    if (created_object)
    {
        auto stmt = _db.query("INSERT INTO object_chunk(object_id, chunk_index, data) VALUES(?1, ?2, ?3)");
        CC_RETURN_IF_ERROR(stmt);

        auto index = i64(0);
        for (auto offset = i64(0); offset < row.size; offset += chunk_size_bytes)
        {
            auto const n = cc::min(chunk_size_bytes, row.size - offset);
            CC_RETURN_IF_ERROR(stmt.value().reset());
            CC_RETURN_IF_ERROR(stmt.value().bind(1, object_id));
            CC_RETURN_IF_ERROR(stmt.value().bind(2, index));
            CC_RETURN_IF_ERROR(stmt.value().bind(3, data.span().subspan({.offset = offset, .size = n})));
            CC_RETURN_IF_ERROR(stmt.value().next());
            CC_RETURN_IF_ERROR(check(stmt.value()));
            ++index;
        }
    }

    // The entry, also first-writer-wins.
    // A loser here must NOT touch the refcount: it added no reference.
    {
        auto stmt = _db.query("INSERT OR IGNORE INTO entries(namespace, key, version, object_id, created_at,"
                              " accessed_at, expires_at, compute_secs, metadata)"
                              " VALUES(?1, ?2, ?3, ?4, ?5, ?5, ?6, ?7, ?8)");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(row.key.space.name)));
        CC_RETURN_IF_ERROR(stmt.value().bind(2, cc::span<byte const>(row.key.key.bytes)));
        CC_RETURN_IF_ERROR(stmt.value().bind(3, i64(row.key.version)));
        CC_RETURN_IF_ERROR(stmt.value().bind(4, object_id));
        CC_RETURN_IF_ERROR(stmt.value().bind(5, row.created_at));
        CC_RETURN_IF_ERROR(stmt.value().bind(6, row.expires_at));
        CC_RETURN_IF_ERROR(stmt.value().bind(7, row.compute_secs));
        // Braced because CC_RETURN_IF_ERROR expands to an if, which would otherwise take this else.
        if (row.metadata.empty())
        {
            CC_RETURN_IF_ERROR(stmt.value().bind_null(8));
        }
        else
        {
            CC_RETURN_IF_ERROR(stmt.value().bind(8, cc::span<byte const>(row.metadata)));
        }
        CC_RETURN_IF_ERROR(stmt.value().next());
        CC_RETURN_IF_ERROR(check(stmt.value()));
    }
    auto const created_entry = _db.changes() == 1;

    if (created_entry)
    {
        auto stmt = _db.query("UPDATE objects SET refcount = refcount + 1 WHERE id = ?1");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, object_id));
        CC_RETURN_IF_ERROR(stmt.value().next());
        CC_RETURN_IF_ERROR(check(stmt.value()));
    }

    CC_RETURN_IF_ERROR(transaction.value().commit());

    if (!created_entry)
        return put_status::already_present;
    return created_object ? put_status::stored : put_status::deduplicated;
}

cc::result<bool> cache_writer::remove_entry(cache_key const& key)
{
    auto transaction = _db.begin_transaction();
    CC_RETURN_IF_ERROR(transaction);

    {
        auto stmt = _db.query("UPDATE objects SET refcount = refcount - 1 WHERE id IN"
                              " (SELECT object_id FROM entries WHERE namespace = ?1 AND key = ?2 AND version = ?3)");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(key.space.name)));
        CC_RETURN_IF_ERROR(stmt.value().bind(2, cc::span<byte const>(key.key.bytes)));
        CC_RETURN_IF_ERROR(stmt.value().bind(3, i64(key.version)));
        CC_RETURN_IF_ERROR(stmt.value().next());
        CC_RETURN_IF_ERROR(check(stmt.value()));
    }

    {
        auto stmt = _db.query("DELETE FROM entries WHERE namespace = ?1 AND key = ?2 AND version = ?3");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(key.space.name)));
        CC_RETURN_IF_ERROR(stmt.value().bind(2, cc::span<byte const>(key.key.bytes)));
        CC_RETURN_IF_ERROR(stmt.value().bind(3, i64(key.version)));
        CC_RETURN_IF_ERROR(stmt.value().next());
        CC_RETURN_IF_ERROR(check(stmt.value()));
    }
    auto const removed = _db.changes() == 1;

    CC_RETURN_IF_ERROR(transaction.value().commit());
    return removed;
}

cc::result<bcache::i64> cache_writer::remove_namespace(cache_namespace const& space)
{
    auto transaction = _db.begin_transaction();
    CC_RETURN_IF_ERROR(transaction);

    {
        auto stmt = _db.query("UPDATE objects SET refcount = refcount -"
                              " (SELECT COUNT(*) FROM entries WHERE entries.object_id = objects.id"
                              "  AND entries.namespace = ?1)"
                              " WHERE id IN (SELECT object_id FROM entries WHERE namespace = ?1)");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(space.name)));
        CC_RETURN_IF_ERROR(stmt.value().next());
        CC_RETURN_IF_ERROR(check(stmt.value()));
    }

    {
        auto stmt = _db.query("DELETE FROM entries WHERE namespace = ?1");
        CC_RETURN_IF_ERROR(stmt);
        CC_RETURN_IF_ERROR(stmt.value().bind(1, cc::string_view(space.name)));
        CC_RETURN_IF_ERROR(stmt.value().next());
        CC_RETURN_IF_ERROR(check(stmt.value()));
    }
    auto const removed = _db.changes();

    CC_RETURN_IF_ERROR(transaction.value().commit());
    return removed;
}

cc::result<bcache::i64> cache_writer::flush_access(cc::map<i64, double> const& pending)
{
    if (pending.empty())
        return i64(0);

    auto transaction = _db.begin_transaction();
    CC_RETURN_IF_ERROR(transaction);

    auto stmt = _db.query("UPDATE entries SET accessed_at = ?1 WHERE id = ?2 AND accessed_at < ?1");
    CC_RETURN_IF_ERROR(stmt);

    auto written = i64(0);
    for (auto const [entry_id, epoch] : pending)
    {
        CC_RETURN_IF_ERROR(stmt.value().reset());
        CC_RETURN_IF_ERROR(stmt.value().bind(1, epoch));
        CC_RETURN_IF_ERROR(stmt.value().bind(2, entry_id));
        CC_RETURN_IF_ERROR(stmt.value().next());
        CC_RETURN_IF_ERROR(check(stmt.value()));
        written += _db.changes();
    }

    CC_RETURN_IF_ERROR(transaction.value().commit());
    return written;
}
} // namespace bcache::impl
