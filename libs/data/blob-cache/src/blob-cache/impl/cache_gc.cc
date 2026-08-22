#include <blob-cache/impl/cache_gc.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

namespace bcache::impl
{
namespace
{
namespace sql = babel::sqlite;

cc::result<cc::unit> check(sql::statement const& stmt)
{
    if (!stmt.is_ok())
        return cc::error(cc::any_error(cc::string(stmt.last_error().message)));
    return cc::unit{};
}

/// Drops a set of entries and decrements what they referenced, in one transaction.
///
/// The order is load-bearing: the refcounts come down while the entries can still be joined to their objects.
/// Reversed, every decrement would find nothing and every object would leak until a full rebuild.
cc::result<i64> delete_entries(sql::database& db, cc::span<i64 const> ids)
{
    if (ids.empty())
        return i64(0);

    // Built rather than bound, because SQLite has no array parameter.
    // The values are rowids this file just read, so there is nothing here a caller could inject.
    auto list = cc::string();
    for (auto const id : ids)
    {
        if (!list.empty())
            list += ",";
        cc::format_append(list, "{}", id);
    }

    auto transaction = db.begin_transaction();
    CC_RETURN_IF_ERROR(transaction);

    CC_RETURN_IF_ERROR(db.exec(cc::format("UPDATE objects SET refcount = refcount -"
                                          " (SELECT COUNT(*) FROM entries WHERE entries.object_id = objects.id"
                                          "  AND entries.id IN ({}))"
                                          " WHERE id IN (SELECT object_id FROM entries WHERE id IN ({}))",
                                          list, list)));

    CC_RETURN_IF_ERROR(db.exec(cc::format("DELETE FROM entries WHERE id IN ({})", list)));
    auto const removed = db.changes();

    CC_RETURN_IF_ERROR(transaction.value().commit());
    return removed;
}

cc::result<cc::vector<i64>> select_ids(sql::statement& stmt)
{
    auto ids = cc::vector<i64>();
    for (auto const row : stmt)
        ids.push_back(row.as_i64(0));
    CC_RETURN_IF_ERROR(check(stmt));
    return ids;
}
} // namespace

double half_life_for(double access_epoch_secs)
{
    return cc::max(access_epoch_secs * 24.0, 3600.0);
}

cc::result<i64> cache_collector::remove_known_expired(cc::span<i64 const> entry_ids)
{
    return delete_entries(_db, entry_ids);
}

cc::result<i64> cache_collector::remove_expired_batch(double now)
{
    // `expires_at IS NOT NULL` matches the partial index, so this is an index range scan and not a table scan.
    auto stmt = _db.query("SELECT id FROM entries WHERE expires_at IS NOT NULL AND expires_at <= ?1 LIMIT ?2");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, now));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, _budget.entries_per_batch));

    auto ids = select_ids(stmt.value());
    CC_RETURN_IF_ERROR(ids);
    return delete_entries(_db, ids.value());
}

cc::result<i64> cache_collector::evict_batch(score_parameters const& params, i64 bytes_to_shed, i64 entries_to_shed)
{
    // A full scan and sort per batch, which is the one collection step whose cost is not bounded by the batch size.
    CC_RECORD_SCOPE("bcache.evict_batch");
    CC_RECORD_ACCUM("bcache.evicted_bytes", cc::rec::unit_bytes, bytes_to_shed);

    // A full scan and sort per batch: the score is computed, and nothing indexes it.
    // COALESCE rather than max(compute_secs, default): the default stands in for an UNKNOWN cost, and a floor would
    // also overwrite a small cost somebody honestly measured, making a cheap entry indistinguishable from one that never said.
    // NULL is what unknown looks like in the column, so the substitution is exactly a COALESCE.
    auto stmt = _db.query("SELECT e.id, e.object_id, o.size FROM entries e JOIN objects o ON o.id = e.object_id"
                          " ORDER BY (COALESCE(e.compute_secs, ?1) / max(o.size, 4096.0))"
                          "        / (1.0 + (?2 - e.accessed_at) / ?3) ASC"
                          " LIMIT ?4");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, params.default_compute_secs));
    CC_RETURN_IF_ERROR(stmt.value().bind(2, params.now));
    CC_RETURN_IF_ERROR(stmt.value().bind(3, params.half_life_secs));
    CC_RETURN_IF_ERROR(stmt.value().bind(4, _budget.entries_per_batch));

    auto ids = cc::vector<i64>();
    auto charged = cc::set<i64>();
    auto scheduled_bytes = i64(0);

    for (auto const row : stmt.value())
    {
        if (!ids.empty() && scheduled_bytes >= bytes_to_shed && ids.size() >= entries_to_shed)
            break;

        ids.push_back(row.as_i64(0));
        if (charged.insert(row.as_i64(1)))
            scheduled_bytes += row.as_i64(2);
    }
    CC_RETURN_IF_ERROR(check(stmt.value()));

    return delete_entries(_db, ids);
}

cc::result<gc_result> cache_collector::reclaim_orphan_batch()
{
    CC_RECORD_SCOPE("bcache.reclaim_orphans");

    // DELETE ... LIMIT needs SQLITE_ENABLE_UPDATE_DELETE_LIMIT, which is not a default build option — so every
    // bounded delete here goes through `id IN (SELECT ... LIMIT ?)`, which works on any build.
    auto stmt = _db.query("SELECT id, size FROM objects WHERE refcount <= 0 LIMIT ?1");
    CC_RETURN_IF_ERROR(stmt);
    CC_RETURN_IF_ERROR(stmt.value().bind(1, _budget.objects_per_batch));

    auto ids = cc::vector<i64>();
    auto out = gc_result();
    for (auto const row : stmt.value())
    {
        ids.push_back(row.as_i64(0));
        out.bytes_reclaimed += row.as_i64(1);
    }
    CC_RETURN_IF_ERROR(check(stmt.value()));

    if (ids.empty())
        return out;

    auto list = cc::string();
    for (auto const id : ids)
    {
        if (!list.empty())
            list += ",";
        cc::format_append(list, "{}", id);
    }

    auto transaction = _db.begin_transaction();
    CC_RETURN_IF_ERROR(transaction);
    // The chunks follow by ON DELETE CASCADE, which cache_schema turns foreign keys on for and then reads back.
    CC_RETURN_IF_ERROR(_db.exec(cc::format("DELETE FROM objects WHERE id IN ({})", list)));
    out.objects_reclaimed = _db.changes();
    CC_RETURN_IF_ERROR(transaction.value().commit());

    return out;
}

void cache_collector::vacuum_slice()
{
    // Best-effort: a file that predates auto_vacuum = INCREMENTAL simply keeps its freelist, which costs disk and nothing else.
    // Nothing above this needs to know.
    (void)_db.exec(cc::format("PRAGMA incremental_vacuum({})", _budget.vacuum_pages));
}
} // namespace bcache::impl
