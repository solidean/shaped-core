#include "store_fixture.hh"

#include <babel-serializer/data/sqlite.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>

/// The SQLite arm of the fixture.
///
/// It reaches for babel::sqlite directly, which no library source here does: fabricating a damaged file on purpose is
/// the one thing a store's own API must never offer, and writing a bad row is the only reliable way to do it.
/// `<sqlite3.h>` still appears nowhere.

namespace vdoc::file::test
{
namespace
{
namespace sql = babel::sqlite;

/// A `.vdoc` file in the OS temp directory, removed when the medium dies.
class sqlite_medium final : public store_medium
{
public:
    sqlite_medium() : _path(cc::temp_file_path("vdoc-conformance", ".vdoc")) {}

    ~sqlite_medium() override
    {
        (void)cc::remove_file(_path);
        (void)cc::remove_file(cc::format("{}-wal", _path)); // WAL leaves two siblings behind
        (void)cc::remove_file(cc::format("{}-shm", _path));
    }

    cc::optional<store_handle> open() override
    {
        auto opened = store::open(_path);

        // The handle is held HERE while the load runs, so a hard failure destroys the store on this thread.
        auto const loaded = wait_for(opened.loaded);
        if (loaded.has_error())
            return {};

        // Fault injection outlives a close, exactly as a revoked permission would.
        if (_blocked)
            apply_write_block();
        return cc::move(opened.store);
    }

    cc::vector<byte> snapshot_bytes() override
    {
        // The file's own bytes, so "unchanged" means byte-identical and not merely equivalent.
        auto stream = cc::file_read_stream_adapter::open(_path);
        if (stream.has_error())
            return {};

        auto reader = stream.value().stream();
        auto const size = reader.size();
        if (size.has_error())
            return {};

        auto out = cc::vector<byte>::create_defaulted(size.value());
        if (reader.read_exact(out).has_error())
            return {};
        return out;
    }

    void set_user_version(i32 version) override
    {
        auto db = require_db();
        REQUIRE(db.set_user_version(version).has_value());
    }

    void add_unknown_table(cc::string_view name) override
    {
        auto db = require_db();
        REQUIRE(db.exec(cc::format("CREATE TABLE {}(id INTEGER PRIMARY KEY)", name)).has_value());
    }

    void add_unknown_column(cc::string_view table, cc::string_view column) override
    {
        auto db = require_db();
        REQUIRE(db.exec(cc::format("ALTER TABLE {} ADD COLUMN {} BLOB", table, column)).has_value());
    }

    bool corrupt_first_op_payload() override
    {
        auto db = require_db();
        auto const target = first_op_with_assignments(db);
        if (!target.has_value())
            return false;

        // One byte, inside the payload the id commits to: the row still decodes, and no longer hashes to its id.
        auto damaged = target.value().assignments;
        damaged[damaged.size() - 1] = byte(u8(damaged[damaged.size() - 1]) ^ 0xFF);
        return write_assignments(db, target.value().hash, damaged);
    }

    bool corrupt_first_op_structurally() override
    {
        auto db = require_db();
        auto const target = first_op_with_assignments(db);
        if (!target.has_value())
            return false;

        // An assignment blob has to start with an encoding tag this build knows; 0xFF never is one.
        auto damaged = cc::vector<byte>::create_defaulted(3);
        damaged[0] = byte(0xFF);
        return write_assignments(db, target.value().hash, damaged);
    }

    void block_writes() override
    {
        _blocked = true;
        apply_write_block();
    }

    void unblock_writes() override
    {
        _blocked = false;

        auto db = require_db();
        REQUIRE(db.exec("DROP TRIGGER IF EXISTS conformance_block_ops").has_value());
        REQUIRE(db.exec("DROP TRIGGER IF EXISTS conformance_block_workspace").has_value());
        REQUIRE(db.exec("DROP TRIGGER IF EXISTS conformance_block_blobs").has_value());
        REQUIRE(db.exec("DROP TRIGGER IF EXISTS conformance_block_chunks").has_value());
    }

    bool delete_first_blob_chunk() override
    {
        auto db = require_db();
        auto const id = first_blob_id(db);
        if (!id.has_value())
            return false;

        auto stmt = db.prepare("DELETE FROM blob_chunk WHERE blob_id = ?1 AND chunk_index ="
                               " (SELECT MAX(chunk_index) FROM blob_chunk WHERE blob_id = ?1)");
        REQUIRE(stmt.has_value());
        REQUIRE(stmt.value().bind(1, id.value()).has_value());
        REQUIRE(stmt.value().next().has_value());
        return db.changes() > 0;
    }

    bool set_first_blob_encoding(cc::string_view encoding) override
    {
        auto db = require_db();
        auto const id = first_blob_id(db);
        if (!id.has_value())
            return false;

        auto stmt = db.prepare("UPDATE blobs SET encoding = ?2 WHERE id = ?1");
        REQUIRE(stmt.has_value());
        REQUIRE(stmt.value().bind(1, id.value()).has_value());
        REQUIRE(stmt.value().bind(2, encoding).has_value());
        REQUIRE(stmt.value().next().has_value());
        return true;
    }

    bool corrupt_first_asset_deps() override
    {
        auto db = require_db();

        // A vdoc value has to start with an encoding tag this build knows; 0xFF never is one.
        auto damaged = cc::vector<byte>::create_defaulted(3);
        damaged[0] = byte(0xFF);

        auto stmt = db.prepare("UPDATE assets SET deps = ?1 WHERE deps IS NOT NULL AND asset_id ="
                               " (SELECT MIN(asset_id) FROM assets WHERE deps IS NOT NULL)");
        REQUIRE(stmt.has_value());
        REQUIRE(stmt.value().bind(1, cc::span<byte const>(damaged)).has_value());
        REQUIRE(stmt.value().next().has_value());
        return db.changes() > 0;
    }

    isize count_blobs() override
    {
        auto db = require_db();
        auto stmt = db.query("SELECT COUNT(*) FROM blobs");
        REQUIRE(stmt.has_value());
        for (auto const row : stmt.value())
            return row.as_i64(0);
        return 0;
    }

private:
    struct op_bytes
    {
        cc::vector<byte> hash;
        cc::vector<byte> assignments;
    };

    /// A second connection, for the things a test does to a file from outside.
    /// The store's own connection lives on its actor and is never reachable from here.
    sql::database require_db()
    {
        auto opened = sql::database::open(_path);
        REQUIRE(opened.has_value());
        auto db = cc::move(opened.value());
        REQUIRE(db.set_busy_timeout(2000).has_value());
        return db;
    }

    static cc::optional<op_bytes> first_op_with_assignments(sql::database& db)
    {
        auto stmt = db.query("SELECT hash, assignments FROM ops WHERE assignments IS NOT NULL"
                             " ORDER BY hash LIMIT 1");
        REQUIRE(stmt.has_value());

        for (auto const row : stmt.value())
            return op_bytes{.hash = cc::vector<byte>::create_copy_of(row.as_blob(0)),
                            .assignments = cc::vector<byte>::create_copy_of(row.as_blob(1))};
        return {};
    }

    /// Lowest rowid, which is the first blob written — deterministic on both arms.
    static cc::optional<i64> first_blob_id(sql::database& db)
    {
        auto stmt = db.query("SELECT id FROM blobs ORDER BY id LIMIT 1");
        REQUIRE(stmt.has_value());
        for (auto const row : stmt.value())
            return row.as_i64(0);
        return {};
    }

    static bool write_assignments(sql::database& db, cc::span<byte const> hash, cc::span<byte const> assignments)
    {
        auto stmt = db.prepare("UPDATE ops SET assignments = ?2 WHERE hash = ?1");
        REQUIRE(stmt.has_value());
        REQUIRE(stmt.value().bind(1, hash).has_value());
        REQUIRE(stmt.value().bind(2, assignments).has_value());
        REQUIRE(stmt.value().next().has_value());
        return true;
    }

    /// A trigger that raises on every insert, which is how a write is made to fail from outside.
    /// The store sees an ordinary statement error and rolls its transaction back, exactly as it would on a full disk.
    void apply_write_block()
    {
        auto db = require_db();
        REQUIRE(db.exec("CREATE TRIGGER IF NOT EXISTS conformance_block_ops BEFORE INSERT ON ops"
                        " BEGIN SELECT RAISE(ABORT, 'writes are blocked'); END")
                    .has_value());
        REQUIRE(db.exec("CREATE TRIGGER IF NOT EXISTS conformance_block_workspace BEFORE INSERT ON workspace"
                        " BEGIN SELECT RAISE(ABORT, 'writes are blocked'); END")
                    .has_value());

        // The blob tables too, because the in-memory arm refuses EVERY write at begin(): without these a blob-only
        // publish would be blocked on one arm and succeed on the other, and a test written against it would pass on
        // one of them for the wrong reason.
        REQUIRE(db.exec("CREATE TRIGGER IF NOT EXISTS conformance_block_blobs BEFORE INSERT ON blobs"
                        " BEGIN SELECT RAISE(ABORT, 'writes are blocked'); END")
                    .has_value());
        REQUIRE(db.exec("CREATE TRIGGER IF NOT EXISTS conformance_block_chunks BEFORE INSERT ON blob_chunk"
                        " BEGIN SELECT RAISE(ABORT, 'writes are blocked'); END")
                    .has_value());
    }

    cc::string _path;
    bool _blocked = false;
};
} // namespace

store_impl sqlite_impl()
{
    return {.name = "sqlite",
            .make_medium = []() -> std::unique_ptr<store_medium> { return std::make_unique<sqlite_medium>(); },
            .is_available = [] { return store::is_file_storage_available(); }};
}
} // namespace vdoc::file::test
