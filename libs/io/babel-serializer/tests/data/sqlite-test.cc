#include <babel-serializer/data/sqlite.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

// These tests compile unconditionally and branch on is_available() at runtime — the babel::sqlite API is always
// present; only the backend is fetched-on-demand. No #if here: that is the always-available-API policy
// (see docs/coding-guidelines.md). The last test pins the not-compiled-in contract directly.

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
    auto ids = cc::vector<cc::i64>();
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
    REQUIRE(stmt.bind(1, cc::i64(2)).has_value());
    auto got = cc::vector<cc::string>();
    for (auto row : stmt)
        got.push_back(cc::string::create_copy_of(row.as_string(0)));
    CHECK(stmt.is_ok());
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "grace");

    // reset + re-bind: id = 3 -> linus
    REQUIRE(stmt.reset().has_value());
    REQUIRE(stmt.bind(1, cc::i64(3)).has_value());
    got.clear();
    for (auto row : stmt)
        got.push_back(cc::string::create_copy_of(row.as_string(0)));
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "linus");

    // a bound value that matches nothing yields an empty result
    REQUIRE(stmt.reset().has_value());
    REQUIRE(stmt.bind(1, cc::i64(999)).has_value());
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

    cc::byte const blob_bytes[] = {cc::byte(0xDE), cc::byte(0xAD), cc::byte(0xBE), cc::byte(0xEF)};
    REQUIRE(ins.bind(1, cc::i64(42)).has_value());
    REQUIRE(ins.bind(2, 3.5).has_value());
    REQUIRE(ins.bind(3, cc::string_view("shaped")).has_value());
    REQUIRE(ins.bind(4, cc::span<cc::byte const>(blob_bytes)).has_value());
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
        CHECK(blob[0] == cc::byte(0xDE));
        CHECK(blob[3] == cc::byte(0xEF));

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

    auto image = cc::vector<cc::byte>();
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

TEST("sqlite - availability contract holds in both build modes")
{
    // The API is always present. Whether the backend was compiled in decides success vs. a runtime error —
    // never a missing symbol or a crash. This test asserts the same contract whichever way babel was built.
    if (sql::is_available())
    {
        CHECK(sql::database::open_memory().has_value());
    }
    else
    {
        CHECK(sql::database::open("x.sqlite").has_error());
        CHECK(sql::database::open_readonly("x.sqlite").has_error());
        CHECK(sql::database::open_memory().has_error());
        CHECK(sql::database::open_blob(cc::span<cc::byte const>()).has_error());
    }
}
