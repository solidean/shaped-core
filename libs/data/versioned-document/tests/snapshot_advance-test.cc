#include "op_graph_corpus.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/snapshot_cache.hh>

/// Advancing a snapshot along a single-parent edge, measured against recomputing one.
///
/// The whole claim is an identity — `surviving(child)` is `surviving(parent)` with the child's assignments applied —
/// so every test here compares an advanced snapshot against a fresh materialization of the same op, byte for byte.
/// The corpus behind it is checked against the brute-force oracle in op_graph_corpus-test.cc first.

using namespace cc::primitive_defines;

using vdoc::entity_id;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::snapshot_cache;
using vdoc::snapshot_document;

using namespace vdoc_test;

namespace
{
[[nodiscard]] snapshot_cache unbounded_cache()
{
    return snapshot_cache({.max_unpinned_entries = 1 << 20});
}
} // namespace

TEST("vdoc - an advanced snapshot equals a recomputed one, over the whole corpus")
{
    auto const corpus = generate_corpus();
    auto ever_advanced = false;

    for (auto const& c : corpus)
        for (auto const& child : c.ops)
        {
            auto const* const o = c.graph.find(child);
            if (o->parents.size() != 1)
                continue;

            auto const parent = o->parents[0];

            auto cache = unbounded_cache();
            cache.install(parent, snapshot_document::create_owning_copy(c.graph.materialize(parent)));

            REQUIRE(vdoc::advance_snapshot(c.graph, cache, parent, child));
            ever_advanced = true;

            // the entry moved: it is at the child now and gone from the parent
            CHECK(cache.contains(child));
            CHECK(!cache.contains(parent));

            auto const* const advanced = cache.find(child);
            REQUIRE(advanced != nullptr);
            CHECK(same_document(advanced->document(), c.graph.materialize(child)));
        }

    CHECK(ever_advanced);
}

TEST("vdoc - advancing down a whole linear run agrees at every step")
{
    // One advance is not the interesting case; a hundred is.
    // Each one appends into the chunk list, and a chunk that reallocated would dangle every view written before it —
    // which shows up here and nowhere else.
    auto graph = op_graph();

    auto ops = cc::vector<op_id>();
    auto head = cc::vector<op_id>();
    for (isize i = 0; i < 100; ++i)
    {
        // rewrite the SAME few paths over and over, so old value bytes really do go dead
        property_write const writes[] = {{.path = path_of("e0", "T", "x"), .value = i},
                                         {.path = path_of("e1", "T", "y"), .value = i * 2},
                                         {.path = path_of(cc::format("e{}", i), "T", "z"), .value = i * 3}};
        auto const next = add_op(graph, head, writes);
        ops.push_back(next);
        head = cc::vector<op_id>{next};
    }

    auto cache = unbounded_cache();
    cache.install(ops[0], snapshot_document::create_owning_copy(graph.materialize(ops[0])), /*pinned =*/true);

    for (isize i = 1; i < ops.size(); ++i)
    {
        REQUIRE(vdoc::advance_snapshot(graph, cache, ops[i - 1], ops[i]));

        auto const* const advanced = cache.find(ops[i]);
        REQUIRE(advanced != nullptr);
        CHECK(same_document(advanced->document(), graph.materialize(ops[i])));

        // a pinned entry stays pinned through the move, or the next advance evicts the thing it needs
        CHECK(cache.is_pinned(ops[i]));
    }

    // and a sweep can still be seeded from it, which is the only reason any of this exists
    auto stats = vdoc::impl::materialize_stats();
    op_id const heads[] = {ops.back()};
    auto const doc = vdoc::impl::materialize(graph, heads, {}, {.cache = &cache, .stats = &stats});
    CHECK(stats.snapshots_used == 1);
    CHECK(stats.ops_walked == 1);
    CHECK(same_document(doc, graph.materialize(ops.back())));
}

TEST("vdoc - a view handed out before an advance still reads afterwards")
{
    // The chunk list exists for exactly this: appending must never move bytes an outstanding view points at.
    auto graph = op_graph();

    property_write const first[] = {{.path = path_of("e0", "T", "x"), .value = 11}};
    auto const a = add_op(graph, {}, first);

    auto cache = unbounded_cache();
    cache.install(a, snapshot_document::create_owning_copy(graph.materialize(a)));

    auto const* const before = cache.find(a);
    REQUIRE(before != nullptr);
    auto const held = before->document().try_get(path_of("e0", "T", "x"))->single();
    CHECK(held.as_i64() == 11);

    op_id const from_a[] = {a};
    property_write const second[] = {{.path = path_of("e1", "T", "y"), .value = 22}};
    auto const b = add_op(graph, from_a, second);

    REQUIRE(vdoc::advance_snapshot(graph, cache, a, b));

    // the value was not overwritten, so its bytes are still live and still where they were
    CHECK(held.as_i64() == 11);
}

TEST("vdoc - advancing across an abstention prunes the entries it empties")
{
    // The fiddly half of clearing a path is what it leaves behind.
    // A component entry with zero properties is a shape a materialization never produces, and a parse would SELECT it —
    // schema found, `$alive` absent so alive, version 0 — and build an all-defaults component out of nothing.
    // So this compares against a fresh materialization, which is the only thing that can catch it.
    auto graph = op_graph();

    auto const p = path_of("e0", "T", "x");
    auto const q = path_of("e0", "T", "y");
    auto const other = path_of("e1", "T", "x");

    property_write const w0[] = {{.path = p, .value = 1}, {.path = q, .value = 2}, {.path = other, .value = 3}};
    auto const a = add_op(graph, {}, w0);

    auto cache = unbounded_cache();
    cache.install(a, snapshot_document::create_owning_copy(graph.materialize(a)));

    // withdraw one property: the component survives, with one property fewer
    op_id const from_a[] = {a};
    property_write const w1[] = {{.path = p, .value = 0, .abstain = true}};
    auto const b = add_op(graph, from_a, w1);

    REQUIRE(vdoc::advance_snapshot(graph, cache, a, b));
    REQUIRE(cache.find(b) != nullptr);
    CHECK(same_document(cache.find(b)->document(), graph.materialize(b)));

    // withdraw the sibling too: now the component AND the entity have to go
    op_id const from_b[] = {b};
    property_write const w2[] = {{.path = q, .value = 0, .abstain = true}};
    auto const c = add_op(graph, from_b, w2);

    REQUIRE(vdoc::advance_snapshot(graph, cache, b, c));
    REQUIRE(cache.find(c) != nullptr);
    CHECK(same_document(cache.find(c)->document(), graph.materialize(c)));

    // stated directly, so a failure says which invariant broke rather than only that two documents differ
    CHECK(cache.find(c)->document().try_get(entity_id::of("e0")) == nullptr);
    CHECK(cache.find(c)->document().try_get(entity_id::of("e1")) != nullptr);
}

TEST("vdoc - advancing refuses anything that is not a single-parent child")
{
    auto graph = op_graph();

    property_write const w0[] = {{.path = path_of("e0", "T", "x"), .value = 1}};
    auto const a = add_op(graph, {}, w0);
    property_write const w1[] = {{.path = path_of("e1", "T", "x"), .value = 2}};
    auto const b = add_op(graph, {}, w1);

    op_id const both[] = {a, b};
    property_write const w2[] = {{.path = path_of("e2", "T", "x"), .value = 3}};
    auto const merge = add_op(graph, both, w2);

    op_id const from_a[] = {a};
    property_write const w3[] = {{.path = path_of("e3", "T", "x"), .value = 4}};
    auto const child_of_a = add_op(graph, from_a, w3);

    auto cache = unbounded_cache();
    cache.install(a, snapshot_document::create_owning_copy(graph.materialize(a)));

    // a merge has two parents, and its surviving set is not its parent's plus its own writes
    CHECK(!vdoc::advance_snapshot(graph, cache, a, merge));

    // b is not a's child at all
    CHECK(!vdoc::advance_snapshot(graph, cache, b, child_of_a));

    // no entry at b to advance from
    CHECK(!vdoc::advance_snapshot(graph, cache, b, add_op(graph, cc::span<op_id const>(&b, 1), w3)));

    // an op the graph does not have
    CHECK(!vdoc::advance_snapshot(graph, cache, a, op_id()));

    // every refusal changed nothing
    CHECK(cache.contains(a));
    CHECK(cache.size() == 1);

    // and the one that is a single-parent child still works
    CHECK(vdoc::advance_snapshot(graph, cache, a, child_of_a));
}

TEST("vdoc - advancing onto a skeleton is refused")
{
    // A skeleton's assignments are gone rather than empty, so advancing onto one would assert an identity on no
    // evidence: surviving(child) == surviving(parent), for a child whose writes nobody can see.
    auto graph = op_graph();

    property_write const w0[] = {{.path = path_of("e0", "T", "x"), .value = 1}};
    auto const a = add_op(graph, {}, w0);

    op_id const from_a[] = {a};
    property_write const w1[] = {{.path = path_of("e0", "T", "x"), .value = 2}};
    auto const b = add_op(graph, from_a, w1);

    REQUIRE(graph.skeletonize(b));

    auto cache = unbounded_cache();
    cache.install(a, snapshot_document::create_owning_copy(graph.materialize(a)));

    CHECK(!vdoc::advance_snapshot(graph, cache, a, b));
    CHECK(cache.contains(a));
}

TEST("vdoc - a rewritten path's dead bytes get reclaimed rather than accumulating")
{
    // Overwriting one path a thousand times strands a thousand old values in the chunk list.
    // The rebuild is what keeps a long editing session from growing its snapshot without bound.
    auto graph = op_graph();

    auto ops = cc::vector<op_id>();
    auto head = cc::vector<op_id>();
    for (isize i = 0; i < 400; ++i)
    {
        property_write const writes[] = {{.path = path_of("e0", "T", "x"), .value = i}};
        auto const next = add_op(graph, head, writes);
        ops.push_back(next);
        head = cc::vector<op_id>{next};
    }

    auto cache = unbounded_cache();
    cache.install(ops[0], snapshot_document::create_owning_copy(graph.materialize(ops[0])));

    for (isize i = 1; i < ops.size(); ++i)
        REQUIRE(vdoc::advance_snapshot(graph, cache, ops[i - 1], ops[i]));

    auto const* const final_doc = cache.find(ops.back());
    REQUIRE(final_doc != nullptr);
    CHECK(same_document(final_doc->document(), graph.materialize(ops.back())));

    // the invariant the rebuild enforces: dead never outgrows live for long
    CHECK(final_doc->dead_byte_size() * 2 <= final_doc->owned_byte_size());

    // one live property of a handful of bytes, so anything near 400 values' worth means nothing was reclaimed
    CHECK(final_doc->owned_byte_size() < 400);
}
