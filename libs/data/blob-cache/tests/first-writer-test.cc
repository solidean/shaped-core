#include "cache_fixture.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

// Entries are immutable and the FIRST committed value wins.
//
// This is what makes the cache safe when two processes compute the same logical key and legitimately produce
// different bytes — two ZIPs of the same files, say, differing only in their timestamps.
// Either is a valid answer, and the mapping must not flap between them.

ASYNC_TEST("bcache keeps the first value written under a key")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    auto const key = key_of("archive", "release");

    auto const first = co_await f.cache().put(key, make_blob("first bytes"));
    CHECK(first.status == put_status::stored);

    auto const second = co_await f.cache().put(key, make_blob("different bytes"));
    CHECK(second.status == put_status::already_present);
    CHECK(second.is_present()); // the entry exists, even though this writer is not the one who made it

    // The hash reported is of what WE offered, so a caller can tell that its own bytes are not the stored ones.
    CHECK(!(second.hash == first.hash));

    CHECK(blob_text((co_await f.cache().get(key)).value().data) == "first bytes");
}

ASYNC_TEST("bcache lets a second connection see the first's entry and lose the race to it")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // Two connections over one file: the multi-writer property, without a second process.
    auto f = cache_fixture();
    (void)co_await f.opened();
    auto const key = key_of("archive", "shared");

    (void)co_await f.cache().put(key, make_blob("written by A"));

    auto second = f.open_second();
    (void)co_await second->opened();

    auto const contended = second->put(key, make_blob("written by B"));
    (void)co_await contended;
    CHECK(contended->try_value()->status == put_status::already_present);

    auto const from_b = second->get(key);
    (void)co_await from_b;
    CHECK(blob_text(from_b->try_value()->value().data) == "written by A");

    // And A still reads what A wrote, which is the same row.
    CHECK(blob_text((co_await f.cache().get(key)).value().data) == "written by A");
}

ASYNC_TEST("bcache invalidate drops an entry and lets a new value take the key")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    auto const key = key_of("archive", "replaceable");

    (void)co_await f.cache().put(key, make_blob("old"));
    CHECK((co_await f.cache().invalidate(key)));
    CHECK(!(co_await f.cache().invalidate(key))); // already gone

    CHECK(!(co_await f.cache().get(key)).has_value());

    // Immutability is per LIVE entry: once the entry is gone the key is free again.
    CHECK((co_await f.cache().put(key, make_blob("new"))).status == put_status::stored);
    CHECK(blob_text((co_await f.cache().get(key)).value().data) == "new");
}

ASYNC_TEST("bcache clear empties one namespace and leaves the others alone")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    (void)co_await f.cache().put(key_of("shader", "a"), make_blob("sa"));
    (void)co_await f.cache().put(key_of("shader", "b"), make_blob("sb"));
    (void)co_await f.cache().put(key_of("mesh", "a"), make_blob("ma"));

    CHECK((co_await f.cache().clear(cache_namespace("shader"))) == 2);

    CHECK(!(co_await f.cache().get(key_of("shader", "a"))).has_value());
    CHECK(!(co_await f.cache().get(key_of("shader", "b"))).has_value());
    CHECK((co_await f.cache().get(key_of("mesh", "a"))).has_value());
}
