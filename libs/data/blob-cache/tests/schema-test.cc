#include "cache_fixture.hh"

#include <babel-serializer/data/sqlite.hh>
#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

// An incompatible file is DISCARDED, never refused.
//
// vdoc::file rightly refuses a format version from the future, because guessing would risk somebody's document.
// A cache holds nothing anyone would miss, so refusing would only strand a caller on a stale file forever — and disposability says throwing it away is always safe.
// That is why there is no migration code in this library.

namespace
{
/// Runs `mutate` against the closed cache file, as any other program holding it might.
void with_raw_database(cc::string_view path, cc::function_ref<void(babel::sqlite::database&)> mutate)
{
    auto db = babel::sqlite::database::open(path);
    REQUIRE(db.has_value());
    mutate(db.value());
}
} // namespace

TEST("bcache discards a file written by a newer format version")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("schema", "before"), make_blob("old world")));
    f.cache().close();

    with_raw_database(f.path(), [](babel::sqlite::database& db) { CHECK(db.set_user_version(99).has_value()); });

    f.reopen();
    CHECK(!f.settle(f.cache().get(key_of("schema", "before"))).has_value());
    CHECK(f.cache().get_stats().is_backed_by_storage);
    CHECK(f.settle(f.cache().put(key_of("schema", "after"), make_blob("new world"))).status == put_status::stored);
}

TEST("bcache discards a file written by an older format version")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // Both directions, and deliberately so: keeping an old file would mean carrying migration code for data that is by definition reconstructible.
    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("schema", "before"), make_blob("old world")));
    f.cache().close();

    with_raw_database(f.path(), [](babel::sqlite::database& db) { CHECK(db.set_user_version(0).has_value()); });

    f.reopen();
    CHECK(!f.settle(f.cache().get(key_of("schema", "before"))).has_value());
    CHECK(f.settle(f.cache().put(key_of("schema", "after"), make_blob("new world"))).status == put_status::stored);
}

TEST("bcache discards a database that belongs to some other application")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    f.cache().close();

    with_raw_database(f.path(),
                      [](babel::sqlite::database& db)
                      {
                          CHECK(db.exec("CREATE TABLE somebody_elses (x INTEGER)").has_value());
                          CHECK(db.set_application_id(0x11223344).has_value());
                      });

    f.reopen();

    CHECK(f.cache().get_stats().is_backed_by_storage);
    CHECK(f.settle(f.cache().put(key_of("schema", "ours"), make_blob("claimed"))).status == put_status::stored);
    CHECK(blob_text(f.settle(f.cache().get(key_of("schema", "ours"))).value().data) == "claimed");
}

TEST("bcache discards a file whose tables no longer have the columns it addresses")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // The version alone cannot catch this: a build that renamed a column without bumping it would otherwise fail obscurely on the first statement instead of starting clean.
    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("schema", "before"), make_blob("old world")));
    f.cache().close();

    with_raw_database(f.path(), [](babel::sqlite::database& db)
                      { CHECK(db.exec("ALTER TABLE entries DROP COLUMN compute_secs").has_value()); });

    f.reopen();
    CHECK(!f.settle(f.cache().get(key_of("schema", "before"))).has_value());
    CHECK(f.settle(f.cache().put(key_of("schema", "after"), make_blob("new world"))).status == put_status::stored);
}

TEST("bcache stamps a fresh file as its own")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("schema", "entry"), make_blob("payload")));
    f.cache().close();

    with_raw_database(f.path(),
                      [](babel::sqlite::database& db)
                      {
                          // The database-level identifier says whose FILE this is; an entry's namespace says which logical partition a row belongs to.
                          // Different concepts, easily conflated.
                          auto const id = db.get_application_id();
                          REQUIRE(id.has_value());
                          CHECK(id.value() == 0x42434845); // 'BCHE'

                          auto const version = db.get_user_version();
                          REQUIRE(version.has_value());
                          CHECK(version.value() == 1);
                      });
}

TEST("bcache keeps a file a newer build added a column to")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // An EXTRA column is a newer build's, and survives because no statement here ever addresses it.
    // Discarding on one would make two builds sharing a machine wipe the cache from each other on every alternate run.
    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("schema", "kept"), make_blob("still here")));
    f.cache().close();

    with_raw_database(f.path(), [](babel::sqlite::database& db)
                      { CHECK(db.exec("ALTER TABLE entries ADD COLUMN future_field TEXT").has_value()); });

    f.reopen();
    CHECK(blob_text(f.settle(f.cache().get(key_of("schema", "kept"))).value().data) == "still here");
}
