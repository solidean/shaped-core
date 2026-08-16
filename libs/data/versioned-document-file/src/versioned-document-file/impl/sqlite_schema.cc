#include <clean-core/container/map.hh>
#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/sqlite_schema.hh>

namespace vdoc::file::impl
{
namespace
{
namespace sql = babel::sqlite;

/// One table this build knows: its DDL, and the columns that must be there for it to be readable.
struct known_table
{
    cc::string_view name;
    cc::string_view ddl;
    /// Every column this build reads or writes.
    /// A missing one is a hard failure; an EXTRA one is a newer build's, and is preserved by never being addressed.
    cc::vector<cc::string_view> columns;
};

cc::vector<known_table> known_tables()
{
    auto tables = cc::vector<known_table>();

    tables.push_back({.name = "ops",
                      .ddl = "CREATE TABLE ops ("
                             " hash        BLOB PRIMARY KEY NOT NULL,"
                             " parents     BLOB NOT NULL,"
                             " metadata    BLOB,"
                             " assignments BLOB"
                             ") WITHOUT ROWID",
                      .columns = {"hash", "parents", "metadata", "assignments"}});

    tables.push_back({.name = "refs",
                      .ddl = "CREATE TABLE refs ("
                             " name    TEXT PRIMARY KEY NOT NULL,"
                             " op_hash BLOB NOT NULL"
                             ") WITHOUT ROWID",
                      .columns = {"name", "op_hash"}});

    // The payload lives in snapshot_chunk rather than inline, because SQLite caps a single value near a gigabyte and a
    // snapshot of a large document goes past that.
    tables.push_back({.name = "snapshots",
                      .ddl = "CREATE TABLE snapshots ("
                             " op_hash      BLOB PRIMARY KEY NOT NULL,"
                             " required     INTEGER NOT NULL,"
                             " encoding     TEXT NOT NULL,"
                             " decoded_size INTEGER NOT NULL,"
                             " stored_size  INTEGER NOT NULL,"
                             " chunk_count  INTEGER NOT NULL"
                             ") WITHOUT ROWID",
                      .columns = {"op_hash", "required", "encoding", "decoded_size", "stored_size", "chunk_count"}});

    // Cascading off `snapshots` rather than living in `blobs` is deliberate.
    // Blob lifetime is decided by a mark-and-sweep over the asset index, and a required snapshot is load-bearing data
    // whose loss is unrecoverable — so a marking bug would become a data-loss bug.
    // Here the lifetime is structural: these bytes die when their snapshot row dies, and nothing else can reach them.
    tables.push_back({.name = "snapshot_chunk",
                      .ddl = "CREATE TABLE snapshot_chunk ("
                             " op_hash     BLOB NOT NULL,"
                             " chunk_index INTEGER NOT NULL,"
                             " data        BLOB NOT NULL,"
                             " PRIMARY KEY(op_hash, chunk_index),"
                             " FOREIGN KEY(op_hash) REFERENCES snapshots(op_hash) ON DELETE CASCADE"
                             ") WITHOUT ROWID",
                      .columns = {"op_hash", "chunk_index", "data"}});

    // `deps` is what lets reclamation compute a closure instead of the application resolving one: an array of asset id
    // strings, declared by the application and never interpreted here.
    tables.push_back({.name = "assets",
                      .ddl = "CREATE TABLE assets ("
                             " asset_id TEXT PRIMARY KEY NOT NULL,"
                             " kind     TEXT NOT NULL,"
                             " parts    BLOB NOT NULL,"
                             " meta     BLOB,"
                             " deps     BLOB"
                             ") WITHOUT ROWID",
                      .columns = {"asset_id", "kind", "parts", "meta", "deps"}});

    // A rowid table, because incremental blob I/O addresses rows by rowid — which is how a chunk is read without
    // materializing it in memory first.
    tables.push_back({.name = "blobs",
                      .ddl = "CREATE TABLE blobs ("
                             " id          INTEGER PRIMARY KEY,"
                             " hash        BLOB NOT NULL UNIQUE,"
                             " size        INTEGER NOT NULL,"
                             " stored_size INTEGER NOT NULL,"
                             " chunk_count INTEGER NOT NULL,"
                             " format      TEXT NOT NULL,"
                             " encoding    TEXT NOT NULL"
                             ")",
                      .columns = {"id", "hash", "size", "stored_size", "chunk_count", "format", "encoding"}});

    tables.push_back({.name = "blob_chunk",
                      .ddl = "CREATE TABLE blob_chunk ("
                             " id          INTEGER PRIMARY KEY,"
                             " blob_id     INTEGER NOT NULL,"
                             " chunk_index INTEGER NOT NULL,"
                             " data        BLOB NOT NULL,"
                             " UNIQUE(blob_id, chunk_index),"
                             " FOREIGN KEY(blob_id) REFERENCES blobs(id) ON DELETE CASCADE"
                             ")",
                      .columns = {"id", "blob_id", "chunk_index", "data"}});

    tables.push_back({.name = "workspace",
                      .ddl = "CREATE TABLE workspace ("
                             " key     TEXT PRIMARY KEY NOT NULL,"
                             " version INTEGER NOT NULL,"
                             " value   BLOB NOT NULL"
                             ") WITHOUT ROWID",
                      .columns = {"key", "version", "value"}});

    tables.push_back({.name = "meta",
                      .ddl = "CREATE TABLE meta ("
                             " key   TEXT PRIMARY KEY NOT NULL,"
                             " value BLOB"
                             ") WITHOUT ROWID",
                      .columns = {"key", "value"}});

    return tables;
}

/// The tables the file actually has, ignoring SQLite's own.
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

/// Templated over the container, so it serves both the cc::string names a query hands back and the string_view
/// literals a known_table lists.
template <class NamesT>
bool contains(NamesT const& names, cc::string_view name)
{
    for (auto const& n : names)
        if (n == name)
            return true;
    return false;
}
} // namespace

cc::result<schema_scan> ensure_schema(sql::database& db)
{
    // Foreign keys default OFF in SQLite, and blob_chunk's cascade is a correctness dependency rather than an
    // assumption — so it is turned on and then READ BACK.
    CC_RETURN_IF_ERROR(db.set_busy_timeout(2000));
    CC_RETURN_IF_ERROR(db.set_foreign_keys(true));
    auto foreign_keys = db.get_foreign_keys();
    CC_RETURN_IF_ERROR(foreign_keys);
    if (!foreign_keys.value())
        return cc::error(cc::any_error(cc::string("foreign keys could not be enabled, and blob_chunk's ON DELETE "
                                                  "CASCADE depends on them")));

    // The FIRST real page read: a file that is not a database, or one that is damaged, surfaces exactly here.
    auto const application_id = db.get_application_id();
    if (application_id.has_error())
    {
        auto const& e = application_id.error();
        if (e.code == sql::error_code::not_a_database)
            return cc::error(cc::any_error(cc::string("not a SQLite database")));
        if (e.code == sql::error_code::corrupt)
            return cc::error(cc::any_error(cc::string("the database image is malformed")));
        return cc::error(cc::any_error(cc::string(e.message)));
    }

    // 0 is a fresh file we may claim; anything else that is not ours is somebody else's file.
    auto const is_fresh = application_id.value() == 0;
    if (!is_fresh && application_id.value() != vdoc_application_id)
        return cc::error(cc::any_error(cc::format("not a .vdoc file: its application_id is 0x{:08X}, not 0x{:08X}",
                                                  u32(application_id.value()), u32(vdoc_application_id))));

    auto user_version = db.get_user_version();
    CC_RETURN_IF_ERROR(user_version);
    if (user_version.value() > current_user_version)
        return cc::error(cc::any_error(cc::format("the file was written by a newer build: format version {}, and this "
                                                  "build knows {}",
                                                  user_version.value(), current_user_version)));

    // WAL is what a file written incrementally wants, but a temp or memory VFS can legitimately refuse it — so this is requested and not insisted on.
    // Outside any transaction, since journal_mode cannot change inside one.
    (void)db.set_journal_mode(sql::journal_mode::wal);

    auto present = read_table_names(db);
    CC_RETURN_IF_ERROR(present);

    auto scan = schema_scan();
    auto const tables = known_tables();

    auto transaction = db.begin_transaction();
    CC_RETURN_IF_ERROR(transaction);

    for (auto const& table : tables)
    {
        if (!contains(present.value(), table.name))
        {
            CC_RETURN_IF_ERROR(db.exec(table.ddl)); // create what is missing, and only that
            continue;
        }

        auto columns = read_column_names(db, table.name);
        CC_RETURN_IF_ERROR(columns);

        // A missing column is HARD, and says which one: this is the alternative to a later statement failing obscurely.
        for (auto const& wanted : table.columns)
            if (!contains(columns.value(), wanted))
                return cc::error(cc::any_error(cc::format("table '{}' is missing column '{}', so this build cannot "
                                                          "read it forward",
                                                          table.name, wanted)));

        // An extra column is a newer build's, and survives because no statement here ever addresses it.
        for (auto const& found : columns.value())
            if (!contains(table.columns, found))
                scan.unknown_columns.push_back(cc::format("{}.{}", table.name, found));
    }

    for (auto const& name : present.value())
    {
        auto is_known = false;
        for (auto const& table : tables)
            is_known = is_known || table.name == name;
        if (!is_known)
            scan.unknown_tables.push_back(name); // reported, then never touched again
    }

    if (is_fresh)
    {
        CC_RETURN_IF_ERROR(db.set_application_id(vdoc_application_id));
        CC_RETURN_IF_ERROR(db.set_user_version(current_user_version));
    }

    CC_RETURN_IF_ERROR(transaction.value().commit());
    return scan;
}
} // namespace vdoc::file::impl
