#pragma once

#include <babel-serializer/data/sqlite.hh>
#include <blob-cache/blob_cache.hh>
#include <blob-cache/impl/cache_rows.hh>
#include <clean-core/container/map.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>

/// The reader and writer over a live connection — the only place in this library that talks to a database.
///
/// Both are constructed on the actor thread, over the actor's own connection, and never leave it.
/// Every statement NAMES ITS COLUMNS, and there is no `SELECT *` anywhere: that is what makes a column a newer build added survive, because nothing on the read or write path can address it.

namespace bcache::impl
{
class cache_reader
{
public:
    explicit cache_reader(babel::sqlite::database& db) : _db(db) {}

    /// The entry for `key`, joined to its object.
    /// Absent rather than failed when there is simply nothing there.
    [[nodiscard]] cc::result<cc::optional<entry_row>> find_entry(cache_key const& key);

    /// Reads an object's payload into one freshly pinned buffer.
    ///
    /// Streams through a blob handle rather than materializing rows: the bytes land directly in the caller's buffer,
    /// so nothing is staged and `row::as_blob`'s "valid only until the next step" hazard never arises.
    [[nodiscard]] cc::result<blob> read_object(entry_row const& entry);

    [[nodiscard]] cc::result<size_totals> read_totals();

private:
    babel::sqlite::database& _db;
};

/// Writes one put as one transaction.
///
/// The transaction lives here rather than in the caller, so that this writer dying — for any reason, on any path — rolls it back.
/// There is no route to an entry that references a half-written object.
class cache_writer
{
public:
    explicit cache_writer(babel::sqlite::database& db) : _db(db) {}

    /// Stores `row`'s object and entry, both first-writer-wins.
    /// `data` is read only when the object turns out to be absent.
    [[nodiscard]] cc::result<put_status> insert(put_row const& row, blob const& data);

    /// Drops one entry by key, decrementing its object's refcount.
    /// False when there was nothing to drop.
    [[nodiscard]] cc::result<bool> remove_entry(cache_key const& key);

    /// Drops every entry in `space`, returning how many.
    [[nodiscard]] cc::result<i64> remove_namespace(cache_namespace const& space);

    /// Writes the buffered access times out, returning how many rows actually changed.
    ///
    /// The `accessed_at < :epoch` guard does two jobs: it never walks recency BACKWARDS past a value another process
    /// recorded, and within one epoch it matches nothing, so SQLite dirties no page at all.
    [[nodiscard]] cc::result<i64> flush_access(cc::map<i64, double> const& pending);

private:
    babel::sqlite::database& _db;
};
} // namespace bcache::impl
