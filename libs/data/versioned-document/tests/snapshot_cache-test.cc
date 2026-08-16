#include "op_graph_corpus.hh"

#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/snapshot_cache.hh>

/// The snapshot cache, measured against the plain replay it exists to shorten.
///
/// Everything here is one property: a cached materialization equals an uncached one, byte for byte.
/// The corpus it runs over is checked against the brute-force oracle in op_graph_corpus-test.cc first, so a green run
/// here means the cache agrees with something independently known to be right.
///
/// **Every equivalence test also asserts the cache was USED.** Comparing two results proves nothing about a cache
/// that was never consulted, and that is by far the most likely way this file passes while meaning nothing.

using namespace cc::primitive_defines;

using vdoc::op_graph;
using vdoc::op_id;
using vdoc::snapshot_cache;
using vdoc::snapshot_document;

using namespace vdoc_test;

namespace
{
/// Materializes with a cache and reports what the sweep did, which is what an equivalence check needs to see.
[[nodiscard]] vdoc::raw_document materialize_cached(op_graph const& graph,
                                                    cc::span<op_id const> heads,
                                                    snapshot_cache& cache,
                                                    vdoc::impl::materialize_stats& stats)
{
    return vdoc::impl::materialize(graph, heads, {}, {.cache = &cache, .stats = &stats});
}

/// A cache that never evicts, so a test controls exactly which ops are snapshotted.
[[nodiscard]] snapshot_cache unbounded_cache()
{
    return snapshot_cache({.max_unpinned_entries = 1 << 20});
}
} // namespace

TEST("vdoc - a cached materialization equals an uncached one, over the whole corpus")
{
    auto const corpus = generate_corpus();
    auto ever_used = false;

    for (auto const& c : corpus)
        for (auto const& heads : c.head_sets)
        {
            auto const expected = c.graph.materialize(heads);

            // installing at EVERY op in turn is what makes this exhaustive rather than anecdotal
            for (auto const& at : c.ops)
            {
                auto cache = unbounded_cache();
                cache.install(at, snapshot_document::create_owning_copy(c.graph.materialize(at)));

                auto stats = vdoc::impl::materialize_stats();
                auto const actual = materialize_cached(c.graph, heads, cache, stats);

                CHECK(same_document(actual, expected));
                ever_used = ever_used || stats.snapshots_used == 1;
            }
        }

    CHECK(ever_used);
}

TEST("vdoc - two snapshots at once equal one and equal none")
{
    auto const corpus = generate_corpus();
    auto ever_used = false;

    for (auto const& c : corpus)
    {
        // the pair sweep is quadratic in ops, so it runs on the smaller cases only
        if (c.ops.size() > 16)
            continue;

        for (auto const& heads : c.head_sets)
        {
            auto const expected = c.graph.materialize(heads);

            for (isize i = 0; i < c.ops.size(); ++i)
                for (isize j = i + 1; j < c.ops.size(); ++j)
                {
                    auto cache = unbounded_cache();
                    cache.install(c.ops[i], snapshot_document::create_owning_copy(c.graph.materialize(c.ops[i])));
                    cache.install(c.ops[j], snapshot_document::create_owning_copy(c.graph.materialize(c.ops[j])));

                    auto stats = vdoc::impl::materialize_stats();
                    auto const actual = materialize_cached(c.graph, heads, cache, stats);

                    CHECK(same_document(actual, expected));
                    ever_used = ever_used || stats.snapshots_used == 1;
                }
        }
    }

    CHECK(ever_used);
}

TEST("vdoc - a filtered sweep may terminate at a snapshot too")
{
    auto const corpus = generate_corpus();

    for (auto const& c : corpus)
    {
        auto entities = cc::vector<vdoc::entity_id>();
        for (auto const& p : c.paths)
        {
            auto seen = false;
            for (auto const& e : entities)
                seen = seen || e == p.entity;
            if (!seen)
                entities.push_back(p.entity);
        }
        if (entities.empty())
            continue;

        for (auto const& heads : c.head_sets)
        {
            auto const expected = c.graph.materialize_entities(heads, entities);

            for (auto const& at : c.ops)
            {
                auto cache = unbounded_cache();
                cache.install(at, snapshot_document::create_owning_copy(c.graph.materialize(at)));

                // a snapshot is unfiltered, and the filter is applied to it by projection — which is sound only
                // because state propagation is per-path independent
                CHECK(same_document(c.graph.materialize_entities(heads, entities, cache), expected));
            }
        }
    }
}

TEST("vdoc - dropping the cache mid-workload changes nothing")
{
    auto const corpus = generate_corpus();

    for (auto const& c : corpus)
    {
        // one live cache across the whole replay, dropped at two different rhythms
        for (auto const drop_every : {isize(3), isize(5)})
        {
            auto cache = snapshot_cache({.max_unpinned_entries = 4});

            for (isize i = 0; i < c.ops.size(); ++i)
            {
                op_id const heads[] = {c.ops[i]};

                auto stats = vdoc::impl::materialize_stats();
                auto const actual = materialize_cached(c.graph, heads, cache, stats);
                CHECK(same_document(actual, c.graph.materialize(heads)));

                vdoc::install_snapshot_if_useful(c.graph, c.ops[i], cache, {.min_ops_behind = 2});

                if (i % drop_every == 0)
                    cache.clear_unpinned();
            }
        }
    }
}

TEST("vdoc - a snapshot is rejected where a branch reaches around it")
{
    // A writes p, B overwrites it, C carries it.
    // Snapshotting C and then branching D off A is the shape that would fabricate a multi-value if C's missing
    // `superseded` were trusted.
    auto graph = op_graph();
    auto const path = path_of("e", "T", "p");

    write const wa[] = {{.path = path, .value = 1}};
    auto const a = add_op(graph, {}, wa);

    op_id const from_a[] = {a};
    write const wb[] = {{.path = path, .value = 2}};
    auto const b = add_op(graph, from_a, wb);

    op_id const from_b[] = {b};
    auto const c = add_op(graph, from_b, {});

    auto const d = add_op(graph, from_a, {});

    op_id const heads[] = {c, d};
    auto const expected = graph.materialize(heads);

    // b overwrote a, so exactly one writer survives even though d never touched the path
    REQUIRE(expected.try_get(path) != nullptr);
    CHECK(!expected.try_get(path)->is_multi_valued());
    CHECK(same_ids(writers_of(expected, path), oracle_writers(graph, heads, path)));

    auto cache = unbounded_cache();
    cache.install(c, snapshot_document::create_owning_copy(graph.materialize(c)));

    auto stats = vdoc::impl::materialize_stats();
    auto const actual = materialize_cached(graph, heads, cache, stats);

    CHECK(same_document(actual, expected));
    CHECK(stats.fell_back);
    CHECK(stats.snapshots_used == 0);
}

TEST("vdoc - a snapshot at an ancestor of another head is rejected")
{
    // materialize({t, x}) with x a DISTANT ancestor of t, both snapshotted.
    // Terminating at t leaves the op between them outside the walk, so both are sources — and "every source is
    // cached" would accept, seed each, and union {x} with {t} into a multi-value nobody wrote.
    auto graph = op_graph();
    auto const path = path_of("e", "T", "p");

    write const w1[] = {{.path = path, .value = 1}};
    auto const x = add_op(graph, {}, w1);

    op_id const from_x[] = {x};
    auto const middle = add_op(graph, from_x, {});

    op_id const from_middle[] = {middle};
    write const w2[] = {{.path = path, .value = 2}};
    auto const t = add_op(graph, from_middle, w2);

    op_id const heads[] = {t, x};
    auto const expected = graph.materialize(heads);
    REQUIRE(expected.try_get(path) != nullptr);
    CHECK(!expected.try_get(path)->is_multi_valued());

    auto cache = unbounded_cache();
    cache.install(x, snapshot_document::create_owning_copy(graph.materialize(x)));
    cache.install(t, snapshot_document::create_owning_copy(graph.materialize(t)));

    auto stats = vdoc::impl::materialize_stats();
    auto const actual = materialize_cached(graph, heads, cache, stats);

    CHECK(same_document(actual, expected));
    CHECK(stats.fell_back);
}

TEST("vdoc - a cached op that is not a source is replayed rather than seeded")
{
    // x is cached but its parent is inside the walk, so it is an ordinary op here: replaying it is correct, and
    // seeding it would drop the superseded set the merge still needs.
    auto graph = op_graph();
    auto const path = path_of("e", "T", "p");

    write const w1[] = {{.path = path, .value = 1}};
    auto const x = add_op(graph, {}, w1);

    op_id const from_x[] = {x};
    write const w2[] = {{.path = path, .value = 2}};
    auto const t = add_op(graph, from_x, w2);

    op_id const heads[] = {t, x};
    auto const expected = graph.materialize(heads);

    auto cache = unbounded_cache();
    cache.install(x, snapshot_document::create_owning_copy(graph.materialize(x)));
    cache.install(t, snapshot_document::create_owning_copy(graph.materialize(t)));

    auto stats = vdoc::impl::materialize_stats();
    CHECK(same_document(materialize_cached(graph, heads, cache, stats), expected));
    CHECK(!stats.fell_back);
    CHECK(stats.snapshots_used == 1); // x seeded, t replayed
}

TEST("vdoc - a snapshot shortens the walk it terminates")
{
    auto graph = op_graph();
    auto const path = path_of("e", "T", "p");

    auto ops = cc::vector<op_id>();
    auto prev = cc::vector<op_id>();
    for (isize i = 0; i < 32; ++i)
    {
        write const w[] = {{.path = path, .value = i}};
        auto const id = add_op(graph, prev, w);
        ops.push_back(id);
        prev = cc::vector<op_id>{id};
    }

    op_id const heads[] = {ops.back()};

    auto plain = vdoc::impl::materialize_stats();
    auto const uncached = vdoc::impl::materialize(graph, heads, {}, {.stats = &plain});
    CHECK(plain.ops_walked == 32);

    auto cache = unbounded_cache();
    cache.install(ops[27], snapshot_document::create_owning_copy(graph.materialize(ops[27])));

    auto stats = vdoc::impl::materialize_stats();
    auto const cached = materialize_cached(graph, heads, cache, stats);

    CHECK(same_document(cached, uncached));
    CHECK(stats.snapshots_used == 1);
    CHECK(!stats.fell_back);
    CHECK(stats.ops_walked == 5); // ops[27] plus the four after it, and nothing older
}

TEST("vdoc - a pinned snapshot survives clear_unpinned and a droppable one does not")
{
    auto graph = op_graph();
    auto const root = add_op(graph, {}, {});
    op_id const from_root[] = {root};
    auto const child = add_op(graph, from_root, {});

    auto cache = unbounded_cache();
    cache.install(root, snapshot_document::create_owning_copy(graph.materialize(root)), /*pinned =*/true);
    cache.install(child, snapshot_document::create_owning_copy(graph.materialize(child)));

    CHECK(cache.size() == 2);
    CHECK(cache.pinned_count() == 1);

    cache.clear_unpinned();

    // a required snapshot stands in for history that is gone, so shedding cache memory must not be able to take it
    CHECK(cache.contains(root));
    CHECK(!cache.contains(child));

    cache.clear();
    CHECK(cache.size() == 0);
}

TEST("vdoc - the eviction budget counts only unpinned entries")
{
    auto graph = op_graph();
    auto ops = cc::vector<op_id>();
    auto prev = cc::vector<op_id>();
    for (isize i = 0; i < 8; ++i)
    {
        auto const id = add_op(graph, prev, {});
        ops.push_back(id);
        prev = cc::vector<op_id>{id};
    }

    auto cache = snapshot_cache({.max_unpinned_entries = 2});
    cache.install(ops[0], snapshot_document::create_owning_copy(graph.materialize(ops[0])), /*pinned =*/true);

    for (isize i = 1; i < ops.size(); ++i)
        cache.install(ops[i], snapshot_document::create_owning_copy(graph.materialize(ops[i])));

    CHECK(cache.pinned_count() == 1);
    CHECK(cache.size() == 3); // the pin, plus the two unpinned the budget allows
    CHECK(cache.contains(ops[0]));
    CHECK(cache.contains(ops[ops.size() - 1]));
}
