#include "cache_fixture.hh"

#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

// Entries are immutable and the FIRST committed value wins.
//
// This is what makes the cache safe when two processes compute the same logical key and legitimately produce
// different bytes — two ZIPs of the same files, say, differing only in their timestamps.
// Either is a valid answer, and the mapping must not flap between them.

TEST("bcache keeps the first value written under a key")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("archive", "release");

    auto const first = f.settle(f.cache().put(key, make_blob("first bytes")));
    CHECK(first.status == put_status::stored);

    auto const second = f.settle(f.cache().put(key, make_blob("different bytes")));
    CHECK(second.status == put_status::already_present);
    CHECK(second.is_present()); // the entry exists, even though this writer is not the one who made it

    // The hash reported is of what WE offered, so a caller can tell that its own bytes are not the stored ones.
    CHECK(!(second.hash == first.hash));

    CHECK(blob_text(f.settle(f.cache().get(key)).value().data) == "first bytes");
}

TEST("bcache lets a second connection see the first's entry and lose the race to it")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // Two connections over one file: the multi-writer property, without a second process.
    auto f = cache_fixture();
    auto const key = key_of("archive", "shared");

    f.settle_only(f.cache().put(key, make_blob("written by A")));

    auto second = f.open_second();
    f.settle_only(second->opened());

    auto const contended = second->put(key, make_blob("written by B"));
    f.settle_only(contended);
    CHECK(contended->try_value()->status == put_status::already_present);

    auto const from_b = second->get(key);
    f.settle_only(from_b);
    CHECK(blob_text(from_b->try_value()->value().data) == "written by A");

    // And A still reads what A wrote, which is the same row.
    CHECK(blob_text(f.settle(f.cache().get(key)).value().data) == "written by A");
}

TEST("bcache invalidate drops an entry and lets a new value take the key")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("archive", "replaceable");

    f.settle_only(f.cache().put(key, make_blob("old")));
    CHECK(f.settle(f.cache().invalidate(key)));
    CHECK(!f.settle(f.cache().invalidate(key))); // already gone

    CHECK(!f.settle(f.cache().get(key)).has_value());

    // Immutability is per LIVE entry: once the entry is gone the key is free again.
    CHECK(f.settle(f.cache().put(key, make_blob("new"))).status == put_status::stored);
    CHECK(blob_text(f.settle(f.cache().get(key)).value().data) == "new");
}

TEST("bcache clear empties one namespace and leaves the others alone")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("shader", "a"), make_blob("sa")));
    f.settle_only(f.cache().put(key_of("shader", "b"), make_blob("sb")));
    f.settle_only(f.cache().put(key_of("mesh", "a"), make_blob("ma")));

    CHECK(f.settle(f.cache().clear(cache_namespace("shader"))) == 2);

    CHECK(!f.settle(f.cache().get(key_of("shader", "a"))).has_value());
    CHECK(!f.settle(f.cache().get(key_of("shader", "b"))).has_value());
    CHECK(f.settle(f.cache().get(key_of("mesh", "a"))).has_value());
}
