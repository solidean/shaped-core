#include <blob-cache/impl/cache_schema.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

namespace bcache::impl
{
namespace
{
namespace sql = babel::sqlite;

/// One table this build knows: its DDL, and the columns that must be there for it to be readable.
struct known_table
{
    cc::string_view name;
    cc::string_view ddl;
    cc::vector<cc::string_view> columns;
};

cc::vector<known_table> known_tables()
{
    auto tables = cc::vector<known_table>();

    // A rowid table so an entry references an object by one integer, which is also what object_chunk joins on.
    // refcount is the live-entry count: 0 makes the object an orphan, and the partial index below is what finds those.
    tables.push_back({.name = "objects",
                      .ddl = "CREATE TABLE objects ("
                             " id          INTEGER PRIMARY KEY,"
                             " hash        BLOB NOT NULL UNIQUE," // cc::hash256::to_bytes, exactly 32 bytes
                             " size        INTEGER NOT NULL,"
                             " chunk_count INTEGER NOT NULL,"
                             " created_at  REAL NOT NULL,"
                             " refcount    INTEGER NOT NULL DEFAULT 0"
                             ")",
                      .columns = {"id", "hash", "size", "chunk_count", "created_at", "refcount"}});

    // MUST be a rowid table: babel::sqlite::blob_handle addresses a cell by rowid and cannot reach a WITHOUT ROWID
    // table at all, which would force every read to materialize a whole chunk as a row value first.
    tables.push_back({.name = "object_chunk",
                      .ddl = "CREATE TABLE object_chunk ("
                             " id          INTEGER PRIMARY KEY,"
                             " object_id   INTEGER NOT NULL,"
                             " chunk_index INTEGER NOT NULL,"
                             " data        BLOB NOT NULL,"
                             " UNIQUE(object_id, chunk_index),"
                             " FOREIGN KEY(object_id) REFERENCES objects(id) ON DELETE CASCADE"
                             ")",
                      .columns = {"id", "object_id", "chunk_index", "data"}});

    // UNIQUE(namespace, key, version) is what makes first-writer-wins one INSERT OR IGNORE rather than a read-then-write race two processes could interleave.
    // A rowid table rather than WITHOUT ROWID on that composite: the access buffer then keys on an integer, so a
    // buffered note costs 16 bytes rather than a whole key, and an eviction batch is `id IN (...)`.
    tables.push_back({.name = "entries",
                      .ddl = "CREATE TABLE entries ("
                             " id           INTEGER PRIMARY KEY,"
                             " namespace    TEXT NOT NULL,"
                             " key          BLOB NOT NULL,"
                             " version      INTEGER NOT NULL,"
                             " object_id    INTEGER NOT NULL,"
                             " created_at   REAL NOT NULL,"
                             " accessed_at  REAL NOT NULL," // quantized to cache_config::access_epoch_secs
                             " expires_at   REAL,"          // NULL means never
                             " compute_secs REAL,"          // NULL means unknown, which is not the same as 0
                             " metadata     BLOB,"
                             " UNIQUE(namespace, key, version),"
                             " FOREIGN KEY(object_id) REFERENCES objects(id)"
                             ")",
                      .columns = {"id", "namespace", "key", "version", "object_id", "created_at", "accessed_at",
                                  "expires_at", "compute_secs", "metadata"}});

    tables.push_back({.name = "cache_meta",
                      .ddl = "CREATE TABLE cache_meta ("
                             " key   TEXT PRIMARY KEY NOT NULL,"
                             " value BLOB"
                             ") WITHOUT ROWID",
                      .columns = {"key", "value"}});

    return tables;
}

/// The indexes, kept apart from the DDL because two of them are PARTIAL and that is the point of having them.
/// Almost no entry expires and almost no object is orphaned, so these cost close to nothing and still turn the expiry and reclamation phases into index scans rather than table scans.
cc::span<cc::string_view const> known_indexes()
{
    static constexpr cc::string_view indexes[] = {
        "CREATE INDEX IF NOT EXISTS entries_by_object ON entries(object_id)",
        "CREATE INDEX IF NOT EXISTS entries_by_expiry ON entries(expires_at) WHERE expires_at IS NOT NULL",
        "CREATE INDEX IF NOT EXISTS entries_by_access ON entries(accessed_at)",
        // `<= 0` rather than `= 0`, to match reclaim_orphan_batch's query exactly.
        // SQLite only uses a partial index where the query's WHERE provably implies the index predicate, and
        // `refcount <= 0` does not imply `refcount = 0` — so the narrower index would sit there unused.
        "CREATE INDEX IF NOT EXISTS objects_orphaned ON objects(refcount) WHERE refcount <= 0",
    };
    return indexes;
}

cc::result<cc::vector<cc::string>> read_table_names(sql::database& db)
{
    auto stmt = db.query("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
    CC_RETURN_IF_ERROR(stmt);

    auto names = cc::vector<cc::string>();
    for (auto const row : stmt.value())
        names.push_back(cc::string(row.as_string(0)));
    if (!stmt.value().is_ok())
        return cc::error(cc::any_error(cc::string(stmt.value().last_error().message)));
    return names;
}

cc::result<cc::vector<cc::string>> read_column_names(sql::database& db, cc::string_view table)
{
    // The table name comes from sqlite_master, never from a caller, so there is nothing here to inject.
    auto stmt = db.query(cc::format("PRAGMA table_info('{}')", table));
    CC_RETURN_IF_ERROR(stmt);

    auto names = cc::vector<cc::string>();
    for (auto const row : stmt.value())
        names.push_back(cc::string(row.as_string(1))); // 0 = cid, 1 = name
    if (!stmt.value().is_ok())
        return cc::error(cc::any_error(cc::string(stmt.value().last_error().message)));
    return names;
}

template <class NamesT>
bool contains(NamesT const& names, cc::string_view name)
{
    for (auto const& n : names)
        if (n == name)
            return true;
    return false;
}

/// Whether every known table is present with every column this build addresses.
/// An EXTRA column is a newer build's and is fine — nothing here addresses it.
cc::result<bool> is_schema_usable(sql::database& db)
{
    auto present = read_table_names(db);
    CC_RETURN_IF_ERROR(present);

    for (auto const& table : known_tables())
    {
        if (!contains(present.value(), table.name))
            return false;

        auto columns = read_column_names(db, table.name);
        CC_RETURN_IF_ERROR(columns);

        for (auto const& wanted : table.columns)
            if (!contains(columns.value(), wanted))
                return false;
    }
    return true;
}

cc::result<cc::unit> create_everything(sql::database& db)
{
    for (auto const& table : known_tables())
        CC_RETURN_IF_ERROR(db.exec(table.ddl));
    for (auto const& index : known_indexes())
        CC_RETURN_IF_ERROR(db.exec(index));
    return cc::unit{};
}
} // namespace

cc::result<cc::unit> recreate_schema(sql::database& db)
{
    auto transaction = db.begin_transaction();
    CC_RETURN_IF_ERROR(transaction);

    // Every table, not only the known ones: a file stamped as ours whose shape we cannot read is ours to reset
    // completely, and leaving a stranger's table behind would only make the next open ambiguous.
    auto present = read_table_names(db);
    CC_RETURN_IF_ERROR(present);
    for (auto const& name : present.value())
        CC_RETURN_IF_ERROR(db.exec(cc::format("DROP TABLE IF EXISTS '{}'", name)));

    CC_RETURN_IF_ERROR(create_everything(db));
    CC_RETURN_IF_ERROR(db.set_application_id(bcache_application_id));
    CC_RETURN_IF_ERROR(db.set_user_version(current_user_version));

    return transaction.value().commit();
}

cc::result<schema_outcome> ensure_schema(sql::database& db)
{
    // A blocked writer waits rather than failing: several processes share this file, and SQLite serializes their
    // writes, so brief contention is the normal case and not an error worth surfacing.
    CC_RETURN_IF_ERROR(db.set_busy_timeout(5000));

    // Foreign keys default OFF in SQLite, and object_chunk's cascade is a correctness dependency rather than an assumption — so it is turned on and then READ BACK.
    CC_RETURN_IF_ERROR(db.set_foreign_keys(true));
    auto foreign_keys = db.get_foreign_keys();
    CC_RETURN_IF_ERROR(foreign_keys);
    if (!foreign_keys.value())
        return cc::error(cc::any_error(cc::string("foreign keys could not be enabled, and object_chunk's ON DELETE "
                                                  "CASCADE depends on them")));

    // WAL is what a file several processes read while one writes wants, but a temp or memory VFS can legitimately refuse it — so this is requested and not insisted on.
    // Outside any transaction, since it cannot change inside one.
    (void)db.set_journal_mode(sql::journal_mode::wal);

    // NORMAL rather than FULL: a power cut may cost the last commits, and losing cache entries is not a failure.
    (void)db.exec("PRAGMA synchronous = NORMAL");

    // BEFORE any table exists — see the header.
    // A no-op on a file that already has tables, which is why the outcome is decided by whether we are about to create them.
    (void)db.exec("PRAGMA auto_vacuum = INCREMENTAL");

    // The FIRST real page read: a file that is not a database, or one that is damaged, surfaces exactly here.
    // Both are reported rather than recovered from — a file we cannot even read a header from is not one to DROP TABLE against, and the caller unlinks it and reopens instead.
    auto const application_id = db.get_application_id();
    if (application_id.has_error())
        return cc::error(cc::any_error(cc::string(application_id.error().message)));

    if (application_id.value() == 0)
    {
        // A fresh file: claim it.
        auto transaction = db.begin_transaction();
        CC_RETURN_IF_ERROR(transaction);
        CC_RETURN_IF_ERROR(create_everything(db));
        CC_RETURN_IF_ERROR(db.set_application_id(bcache_application_id));
        CC_RETURN_IF_ERROR(db.set_user_version(current_user_version));
        CC_RETURN_IF_ERROR(transaction.value().commit());
        return schema_outcome::created_fresh;
    }

    auto const user_version = db.get_user_version();
    CC_RETURN_IF_ERROR(user_version);

    auto discard = application_id.value() != bcache_application_id;
    discard = discard || user_version.value() != current_user_version; // either direction, see the header
    if (!discard)
    {
        auto usable = is_schema_usable(db);
        CC_RETURN_IF_ERROR(usable);
        discard = !usable.value();
    }

    if (!discard)
        return schema_outcome::opened_existing;

    CC_RETURN_IF_ERROR(recreate_schema(db));
    return schema_outcome::discarded_and_recreated;
}
} // namespace bcache::impl
