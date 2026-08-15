#include <babel-serializer/data/sqlite.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/file_path.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// These tests compile unconditionally and branch on is_available() at runtime, with no #if anywhere.
// That is the always-available-API policy in docs/coding-guidelines.md.
// The last test pins the not-compiled-in contract directly.

namespace
{
namespace sql = babel::sqlite;

// A tiny fixture: an in-memory database with a two-column table and three rows.
sql::database make_people()
{
    auto opened = sql::database::open_memory();
    REQUIRE(opened.has_value());
    auto db = cc::move(opened.value());

    REQUIRE(db.exec("CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT)").has_value());
    REQUIRE(db.exec("INSERT INTO people(id, name) VALUES (1, 'ada'), (2, 'grace'), (3, 'linus')").has_value());
    return db;
}

// A repeating byte pattern, so a read at an offset can be checked against the offset it came from.
cc::vector<byte> pattern_bytes(isize length)
{
    cc::vector<byte> data;
    data.resize_to_uninitialized(length);
    for (isize i = 0; i < length; ++i)
        data[i] = byte(i % 251);
    return data;
}

// The .vdoc shape in miniature: blobs, chunks pointing at them, and a cascade between the two.
// Both are rowid tables, which is what lets a chunk be reached by incremental blob I/O at all.
sql::database make_chunks()
{
    auto opened = sql::database::open_memory();
    REQUIRE(opened.has_value());
    auto db = cc::move(opened.value());

    REQUIRE(db.exec("CREATE TABLE blobs(id INTEGER PRIMARY KEY)").has_value());
    REQUIRE(db.exec("CREATE TABLE chunks(id INTEGER PRIMARY KEY, blob_id INTEGER NOT NULL, data BLOB NOT NULL,"
                    " FOREIGN KEY(blob_id) REFERENCES blobs(id) ON DELETE CASCADE)")
                .has_value());
    REQUIRE(db.exec("INSERT INTO blobs(id) VALUES (1), (2)").has_value());

    auto insert_r = db.prepare("INSERT INTO chunks(id, blob_id, data) VALUES (?1, ?2, ?3)");
    REQUIRE(insert_r.has_value());
    auto insert = cc::move(insert_r.value());

    // chunk 1 is 1000 bytes on blob 1, chunk 2 is 40 bytes on blob 2
    for (auto const& [id, length] : {cc::pair<i64, isize>(1, 1000), cc::pair<i64, isize>(2, 40)})
    {
        auto const data = pattern_bytes(length);
        REQUIRE(insert.reset().has_value());
        REQUIRE(insert.bind(1, id).has_value());
        REQUIRE(insert.bind(2, id).has_value());
        REQUIRE(insert.bind(3, cc::span<byte const>(data)).has_value());
        REQUIRE(insert.next().has_value());
    }

    return db;
}

isize count_rows(sql::database& db, cc::string_view sql)
{
    auto stmt_r = db.query(sql);
    REQUIRE(stmt_r.has_value());
    auto stmt = cc::move(stmt_r.value());

    isize count = 0;
    for (auto row : stmt)
        count = isize(row.as_i64(0));
    return count;
}

isize count_people(sql::database& db)
{
    return count_rows(db, "SELECT COUNT(*) FROM people");
}
isize count_chunks(sql::database& db)
{
    return count_rows(db, "SELECT COUNT(*) FROM chunks");
}

bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

TEST("sqlite - open, exec and query")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error()); // no backend: every op errors (see the availability test)
        return;
    }

    auto db = make_people();
    CHECK(db.changes() == 3); // the multi-row INSERT

    auto stmt_r = db.query("SELECT id, name FROM people ORDER BY id");
    REQUIRE(stmt_r.has_value());
    auto stmt = cc::move(stmt_r.value());

    auto names = cc::vector<cc::string>();
    auto ids = cc::vector<i64>();
    for (auto row : stmt)
    {
        CHECK(row.column_count() == 2);
        ids.push_back(row.as_i64(0));
        names.push_back(cc::string::create_copy_of(row.as_string(1)));
        CHECK(row.column_type(0) == sql::column_kind::integer);
        CHECK(row.column_type(1) == sql::column_kind::text);
    }
    CHECK(stmt.is_ok());

    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == 1);
    CHECK(ids[2] == 3);
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "ada");
    CHECK(names[1] == "grace");
    CHECK(names[2] == "linus");
}

TEST("sqlite - prepared statement with binding and reset")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    auto db = make_people();

    auto stmt_r = db.prepare("SELECT name FROM people WHERE id = ?1");
    REQUIRE(stmt_r.has_value());
    auto stmt = cc::move(stmt_r.value());

    // first execution: id = 2 -> grace
    REQUIRE(stmt.bind(1, i64(2)).has_value());
    auto got = cc::vector<cc::string>();
    for (auto row : stmt)
        got.push_back(cc::string::create_copy_of(row.as_string(0)));
    CHECK(stmt.is_ok());
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "grace");

    // reset + re-bind: id = 3 -> linus
    REQUIRE(stmt.reset().has_value());
    REQUIRE(stmt.bind(1, i64(3)).has_value());
    got.clear();
    for (auto row : stmt)
        got.push_back(cc::string::create_copy_of(row.as_string(0)));
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "linus");

    // a bound value that matches nothing yields an empty result
    REQUIRE(stmt.reset().has_value());
    REQUIRE(stmt.bind(1, i64(999)).has_value());
    auto count = 0;
    for (auto row : stmt)
    {
        (void)row;
        ++count;
    }
    CHECK(count == 0);
}

TEST("sqlite - typed columns: integer, real, text, blob, null")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    auto opened = sql::database::open_memory();
    REQUIRE(opened.has_value());
    auto db = cc::move(opened.value());

    REQUIRE(db.exec("CREATE TABLE t(i INTEGER, r REAL, s TEXT, b BLOB, n INTEGER)").has_value());

    auto ins_r = db.prepare("INSERT INTO t(i, r, s, b, n) VALUES (?1, ?2, ?3, ?4, ?5)");
    REQUIRE(ins_r.has_value());
    auto ins = cc::move(ins_r.value());

    byte const blob_bytes[] = {byte(0xDE), byte(0xAD), byte(0xBE), byte(0xEF)};
    REQUIRE(ins.bind(1, i64(42)).has_value());
    REQUIRE(ins.bind(2, 3.5).has_value());
    REQUIRE(ins.bind(3, cc::string_view("shaped")).has_value());
    REQUIRE(ins.bind(4, cc::span<byte const>(blob_bytes)).has_value());
    REQUIRE(ins.bind_null(5).has_value());
    // a statement with no result rows: stepping once returns "no row"
    auto stepped = ins.next();
    REQUIRE(stepped.has_value());
    CHECK(stepped.value() == false);
    CHECK(db.last_insert_rowid() == 1);

    auto sel_r = db.query("SELECT i, r, s, b, n FROM t");
    REQUIRE(sel_r.has_value());
    auto sel = cc::move(sel_r.value());

    auto rows = 0;
    for (auto row : sel)
    {
        ++rows;
        CHECK(row.as_i64(0) == 42);
        CHECK(row.as_double(1) == 3.5);
        CHECK(row.as_string(2) == "shaped");

        auto blob = row.as_blob(3);
        REQUIRE(blob.size() == 4);
        CHECK(blob[0] == byte(0xDE));
        CHECK(blob[3] == byte(0xEF));

        CHECK(row.is_null(4));
        CHECK(row.column_type(4) == sql::column_kind::null);
    }
    CHECK(rows == 1);
}

TEST("sqlite - serialize round-trips through open_blob")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    auto image = cc::vector<byte>();
    {
        auto db = make_people();
        image = db.serialize();
    }
    REQUIRE(image.size() > 0);

    auto reopened = sql::database::open_blob(image);
    REQUIRE(reopened.has_value());
    auto db = cc::move(reopened.value());

    auto stmt_r = db.query("SELECT count(*) FROM people");
    REQUIRE(stmt_r.has_value());
    auto stmt = cc::move(stmt_r.value());

    auto stepped = stmt.next();
    REQUIRE(stepped.has_value());
    REQUIRE(stepped.value() == true);
    CHECK(stmt.current().as_i64(0) == 3);
}

TEST("sqlite - error paths")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    // opening a non-existent database read-only fails
    CHECK(sql::database::open_readonly("./does-not-exist-shaped.sqlite").has_error());

    auto db = make_people();

    // malformed SQL fails at prepare
    CHECK(db.prepare("SELECT FROM WHERE nonsense").has_error());
    // exec of malformed SQL fails
    CHECK(db.exec("NOT VALID SQL").has_error());
    // querying a missing table fails
    CHECK(db.query("SELECT * FROM no_such_table").has_error());
}

TEST("sqlite - incremental blob reads")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    auto db = make_chunks();

    // 1000 bytes, so a 256-byte read at offset 200 spans the boundary a chunked reader would cut at.
    auto handle_r = db.open_blob_handle({.table = "chunks", .column = "data", .rowid = 1});
    REQUIRE(handle_r.has_value());
    auto handle = cc::move(handle_r.value());

    CHECK(handle.size() == 1000);

    auto buffer = cc::vector<byte>();
    buffer.resize_to_defaulted(256);
    REQUIRE(handle.read_at(200, buffer).has_value());
    for (isize i = 0; i < buffer.size(); ++i)
        CHECK(u8(buffer[i]) == u8((200 + i) % 251));

    // the whole value in one go, and a zero-length read at the very end
    auto whole = cc::vector<byte>();
    whole.resize_to_defaulted(1000);
    REQUIRE(handle.read_at(0, whole).has_value());
    CHECK(handle.read_at(1000, cc::span<byte>()).has_value());

    // a range that runs past the end is an error, not a short read
    CHECK(handle.read_at(900, buffer).has_error());
    CHECK(handle.read_at(-1, buffer).has_error());

    // reopen walks to the next row without opening a second handle
    REQUIRE(handle.reopen(2).has_value());
    CHECK(handle.size() == 40);
    REQUIRE(handle.read_at(0, cc::span<byte>(buffer.data(), 40)).has_value());
    for (isize i = 0; i < 40; ++i)
        CHECK(u8(buffer[i]) == u8(i % 251));
}

TEST("sqlite - a committed transaction publishes its rows")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    auto db = make_people();

    {
        auto tx_r = db.begin_transaction();
        REQUIRE(tx_r.has_value());
        auto tx = cc::move(tx_r.value());
        CHECK(tx.is_open());

        REQUIRE(db.exec("INSERT INTO people(id, name) VALUES (4, 'ken')").has_value());
        REQUIRE(tx.commit().has_value());
        CHECK(!tx.is_open());

        // committing twice is an error rather than a silent no-op
        CHECK(tx.commit().has_error());
    }

    CHECK(count_people(db) == 4);
}

TEST("sqlite - an abandoned transaction leaves the database byte-identical")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    auto db = make_people();
    auto const before = db.serialize();
    REQUIRE(!before.empty());

    // The early return is the point: nothing here says "roll back", and the write still does not land.
    auto const abandoned = [&]
    {
        auto tx_r = db.begin_transaction();
        REQUIRE(tx_r.has_value());
        auto tx = cc::move(tx_r.value());

        REQUIRE(db.exec("INSERT INTO people(id, name) VALUES (4, 'ken')").has_value());
        REQUIRE(db.exec("DELETE FROM people WHERE id = 1").has_value());
        return; // tx dies here, unommitted
    };
    abandoned();

    CHECK(count_people(db) == 3);
    CHECK(same_bytes(db.serialize(), before));
}

TEST("sqlite - the chunk cascade follows foreign_keys")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    // OFF is SQLite's default, so a schema that depends on the cascade has to say so.
    {
        auto db = make_chunks();
        auto const enabled = db.get_foreign_keys();
        REQUIRE(enabled.has_value());
        CHECK(!enabled.value());

        REQUIRE(db.exec("DELETE FROM blobs WHERE id = 1").has_value());
        CHECK(count_chunks(db) == 2); // orphaned, not deleted
    }

    {
        auto db = make_chunks();
        REQUIRE(db.set_foreign_keys(true).has_value());
        auto const enabled = db.get_foreign_keys();
        REQUIRE(enabled.has_value());
        CHECK(enabled.value());

        REQUIRE(db.exec("DELETE FROM blobs WHERE id = 1").has_value());
        CHECK(count_chunks(db) == 1); // the cascade took row 1's chunk with it
    }
}

TEST("sqlite - journal mode is reported back, not assumed")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    auto db = make_people();

    // An in-memory database is always `memory` and cannot be talked into WAL — which is exactly why the mode is
    // read back rather than assumed to be whatever was requested.
    auto const initial = db.get_journal_mode();
    REQUIRE(initial.has_value());
    CHECK(initial.value() == sql::journal_mode::memory);

    REQUIRE(db.set_journal_mode(sql::journal_mode::wal).has_value());
    auto const after = db.get_journal_mode();
    REQUIRE(after.has_value());
    CHECK(after.value() == sql::journal_mode::memory);

    CHECK(db.set_busy_timeout(250).has_value());
}

TEST("sqlite - a failure carries a code, not just a message")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    // Junk handed to open_blob is taken as a database image, but nothing is parsed until a page is read.
    // So the situation surfaces at the first read rather than at the open — which is why a caller that must
    // distinguish "not ours" from "damaged" reads a header pragma first instead of trusting a successful open.
    byte junk[64] = {};
    for (isize i = 0; i < isize(sizeof(junk)); ++i)
        junk[i] = byte('x');
    auto opened = sql::database::open_blob(junk);
    REQUIRE(opened.has_value());
    auto junk_db = cc::move(opened.value());

    auto const first_read = junk_db.get_user_version();
    REQUIRE(first_read.has_error());
    CHECK(first_read.error().code == sql::error_code::not_a_database);
    CHECK(first_read.error().native_code != 0);
    CHECK(!first_read.error().message.empty());

    auto db = make_people();

    // A UNIQUE violation is a constraint, and a caller retries or reports it rather than treating it as corruption.
    auto const duplicate = db.exec("INSERT INTO people(id, name) VALUES (1, 'ada')");
    REQUIRE(duplicate.has_error());
    CHECK(duplicate.error().code == sql::error_code::constraint);

    // Bad SQL is the caller's bug, and lands in one bucket with the other caller-side mistakes.
    CHECK(db.prepare("SELECT FROM WHERE nonsense").error().code == sql::error_code::misuse);
    CHECK(db.query("SELECT * FROM no_such_table").error().code == sql::error_code::misuse);

    // A missing file is a distinct situation from a damaged one.
    CHECK(sql::database::open_readonly("./does-not-exist-shaped.sqlite").error().code == sql::error_code::cannot_open);
}

TEST("sqlite - a lock another connection holds reports busy")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    // Two connections need a real file: an in-memory database is never shared.
    auto const path = cc::temp_file_path("babel-sqlite-busy", ".sqlite");

    {
        auto writer_r = sql::database::open(path);
        REQUIRE(writer_r.has_value());
        auto writer = cc::move(writer_r.value());
        REQUIRE(writer.exec("CREATE TABLE t(id INTEGER PRIMARY KEY)").has_value());

        auto other_r = sql::database::open(path);
        REQUIRE(other_r.has_value());
        auto other = cc::move(other_r.value());
        REQUIRE(other.set_busy_timeout(0).has_value()); // do not wait, so the test is not a sleep

        auto held = writer.begin_transaction();
        REQUIRE(held.has_value());
        REQUIRE(writer.exec("INSERT INTO t(id) VALUES (1)").has_value()); // takes the write lock

        auto const blocked = other.exec("INSERT INTO t(id) VALUES (2)");
        REQUIRE(blocked.has_error());
        CHECK(blocked.error().code == sql::error_code::busy);
    }

    CHECK(cc::remove_file(path));
}

TEST("sqlite - application_id and user_version survive a reopen")
{
    if (!sql::is_available())
    {
        CHECK(sql::database::open_memory().has_error());
        return;
    }

    auto const path = cc::temp_file_path("babel-sqlite-header", ".sqlite");
    constexpr i32 vdoc_application_id = 0x56444F43; // 'VDOC', the .vdoc format's own stamp

    {
        auto created_r = sql::database::open(path);
        REQUIRE(created_r.has_value());
        auto created = cc::move(created_r.value());

        // A database nobody has stamped reads as zero on both fields, which is how a fresh file is recognized.
        REQUIRE(created.get_application_id().has_value());
        CHECK(created.get_application_id().value() == 0);
        REQUIRE(created.get_user_version().has_value());
        CHECK(created.get_user_version().value() == 0);

        REQUIRE(created.set_application_id(vdoc_application_id).has_value());
        REQUIRE(created.set_user_version(7).has_value());
        REQUIRE(created.exec("CREATE TABLE t(id INTEGER PRIMARY KEY)").has_value());
    }

    {
        // Both live in the file header rather than in a table, so they come back without anything being read.
        auto reopened_r = sql::database::open(path);
        REQUIRE(reopened_r.has_value());
        auto reopened = cc::move(reopened_r.value());

        REQUIRE(reopened.get_application_id().has_value());
        CHECK(reopened.get_application_id().value() == vdoc_application_id);
        REQUIRE(reopened.get_user_version().has_value());
        CHECK(reopened.get_user_version().value() == 7);
    }

    CHECK(cc::remove_file(path));
}

TEST("sqlite - availability contract holds in both build modes")
{
    // Whether the backend was compiled in decides success vs. a runtime error, never a missing symbol or a crash.
    // This test asserts the same contract whichever way babel was built.
    if (sql::is_available())
    {
        CHECK(sql::database::open_memory().has_value());
    }
    else
    {
        CHECK(sql::database::open("x.sqlite").has_error());
        CHECK(sql::database::open_readonly("x.sqlite").has_error());
        CHECK(sql::database::open_memory().has_error());
        CHECK(sql::database::open_blob(cc::span<byte const>()).has_error());

        // A database is never handed out without a backend, so a default-constructed one is what a caller would
        // reach the new entry points through — each reports rather than crashing.
        auto db = sql::database();
        CHECK(db.open_blob_handle({.table = "t", .column = "c", .rowid = 1}).has_error());
        CHECK(db.begin_transaction().has_error());
        CHECK(db.set_journal_mode(sql::journal_mode::wal).has_error());
        CHECK(db.get_journal_mode().has_error());
        CHECK(db.set_busy_timeout(0).has_error());
        CHECK(db.set_foreign_keys(true).has_error());
        CHECK(db.get_foreign_keys().has_error());
        CHECK(db.get_application_id().has_error());
        CHECK(db.set_application_id(1).has_error());
        CHECK(db.get_user_version().has_error());
        CHECK(db.set_user_version(1).has_error());

        // Absent-backend failures are the one situation SQLite was never asked about, so they say so.
        CHECK(db.get_user_version().error().code == sql::error_code::backend_missing);
        CHECK(db.get_user_version().error().native_code == 0);

        auto handle = sql::blob_handle();
        CHECK(handle.size() == 0);
        CHECK(handle.read_at(0, cc::span<byte>()).has_error());
        CHECK(handle.reopen(1).has_error());

        auto tx = sql::transaction();
        CHECK(!tx.is_open());
        CHECK(tx.commit().has_error());
    }
}
