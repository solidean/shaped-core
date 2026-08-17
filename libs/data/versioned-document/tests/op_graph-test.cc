#include "op_graph_corpus.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <versioned-document/op_graph.hh>

using namespace cc::primitive_defines;

using vdoc::entity_id;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::property_path;

using vdoc_test::add_op;
using vdoc_test::oracle_writers;
using vdoc_test::path_of;
using vdoc_test::property_write;
using vdoc_test::same_ids;
using vdoc_test::writers_of;

TEST("vdoc - add is idempotent and does not disturb the child index")
{
    auto graph = op_graph();

    auto const root = add_op(graph, {}, {});
    op_id const parents[] = {root};
    property_write const writes[] = {{.path = path_of("e1", "T", "x"), .value = 1}};
    auto const child = add_op(graph, parents, writes);

    CHECK(graph.size() == 2);
    CHECK(graph.children(root).size() == 1);

    // re-adding the same content must change nothing at all, or a duplicate add would list the child twice
    auto const again = add_op(graph, parents, writes);
    CHECK(again == child);
    CHECK(graph.size() == 2);
    CHECK(graph.children(root).size() == 1);
}

TEST("vdoc - linear history resolves last-write-wins")
{
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");

    property_write const first[] = {{.path = p, .value = 1}};
    auto const a = add_op(graph, {}, first);

    op_id const from_a[] = {a};
    property_write const second[] = {{.path = p, .value = 2}};
    auto const b = add_op(graph, from_a, second);

    auto const doc = graph.materialize(b);
    auto const* const prop = doc.try_get(p);
    REQUIRE(prop != nullptr);

    CHECK(!prop->is_multi_valued());
    CHECK(prop->single().as_i64() == 2);
    CHECK(prop->writers[0].writer == b);
}

TEST("vdoc - a diamond where both sides write one path leaves two values")
{
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");

    auto const root = add_op(graph, {}, {});
    op_id const from_root[] = {root};

    property_write const left_write[] = {{.path = p, .value = 1}};
    property_write const right_write[] = {{.path = p, .value = 2}};
    auto const left = add_op(graph, from_root, left_write);
    auto const right = add_op(graph, from_root, right_write);

    op_id const both[] = {left, right};
    auto const merge = add_op(graph, both, {});

    auto const doc = graph.materialize(merge);
    auto const* const prop = doc.try_get(p);
    REQUIRE(prop != nullptr);

    // neither dominates the other, so storage keeps both and refuses to pick
    CHECK(prop->is_multi_valued());
    CHECK(prop->writers.size() == 2);
    CHECK(same_ids(writers_of(doc, p), oracle_writers(graph, both, p)));
}

TEST("vdoc - concurrent writers of the SAME bytes still leave two values")
{
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");

    auto const root = add_op(graph, {}, {});
    op_id const from_root[] = {root};

    // byte-identical writes: the parse layer collapses this silently, and storage must not
    property_write const same[] = {{.path = p, .value = 7}};
    auto const left = add_op(graph, from_root, same);
    auto const right_parent = add_op(graph, from_root, {});
    op_id const from_right[] = {right_parent};
    auto const right = add_op(graph, from_right, same);

    op_id const both[] = {left, right};
    auto const doc = graph.materialize(both);
    auto const* const prop = doc.try_get(p);
    REQUIRE(prop != nullptr);

    CHECK(prop->is_multi_valued());
    CHECK(prop->writers.size() == 2);
    CHECK(prop->writers[0].value.as_i64() == 7);
    CHECK(prop->writers[1].value.as_i64() == 7);

    // what happened is two independent writes, and that is what storage records
    CHECK(!(prop->writers[0].writer == prop->writers[1].writer));
}

TEST("vdoc - a superseded writer is not resurrected across a second merge")
{
    // The case a plain union gets wrong even after the simple diamond passes.
    // A and B write p concurrently; X merges them AND writes p, so it supersedes both; Y merges them and writes
    // nothing.
    // Merging X and Y must leave only X.
    //
    // An implementation that supersedes on the apply step but drops the set at merges passes the diamond above and
    // fails here.
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");

    auto const root = add_op(graph, {}, {});
    op_id const from_root[] = {root};

    property_write const a_write[] = {{.path = p, .value = 1}};
    property_write const b_write[] = {{.path = p, .value = 2}};
    auto const a = add_op(graph, from_root, a_write);
    auto const b = add_op(graph, from_root, b_write);

    op_id const ab[] = {a, b};
    property_write const x_write[] = {{.path = p, .value = 3}};
    auto const x = add_op(graph, ab, x_write);
    auto const y = add_op(graph, ab, {});

    op_id const xy[] = {x, y};
    auto const doc = graph.materialize(xy);
    auto const* const prop = doc.try_get(p);
    REQUIRE(prop != nullptr);

    CHECK(!prop->is_multi_valued());
    CHECK(prop->single().as_i64() == 3);
    CHECK(prop->writers[0].writer == x);
    CHECK(same_ids(writers_of(doc, p), oracle_writers(graph, xy, p)));
}

TEST("vdoc - materializing an ancestor alongside its descendant equals the descendant alone")
{
    // The multi-head combine is a separate code path from the per-op merge, and is where a naive union gets written.
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");

    property_write const first[] = {{.path = p, .value = 1}};
    auto const a = add_op(graph, {}, first);

    op_id const from_a[] = {a};
    property_write const second[] = {{.path = p, .value = 2}};
    auto const b = add_op(graph, from_a, second);

    op_id const both[] = {a, b};
    op_id const only_b[] = {b};

    CHECK(same_ids(writers_of(graph.materialize(both), p), writers_of(graph.materialize(only_b), p)));
    CHECK(graph.materialize(both).try_get(p)->single().as_i64() == 2);
}

TEST("vdoc - materializing several heads equals materializing a merge op over them")
{
    auto graph = op_graph();

    auto const left_path = path_of("e1", "T", "x");
    auto const right_path = path_of("e2", "T", "y");

    auto const root = add_op(graph, {}, {});
    op_id const from_root[] = {root};

    property_write const left_write[] = {{.path = left_path, .value = 1}};
    property_write const right_write[] = {{.path = right_path, .value = 2}};
    auto const left = add_op(graph, from_root, left_write);
    auto const right = add_op(graph, from_root, right_write);

    op_id const both[] = {left, right};
    auto const merge = add_op(graph, both, {});

    auto const from_heads = graph.materialize(both);
    auto const from_merge = graph.materialize(merge);

    CHECK(same_ids(writers_of(from_heads, left_path), writers_of(from_merge, left_path)));
    CHECK(same_ids(writers_of(from_heads, right_path), writers_of(from_merge, right_path)));
    CHECK(from_heads.property_count() == from_merge.property_count());
}

TEST("vdoc - reachability skips a pruned parent rather than failing")
{
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");

    // a parent that was never added, standing in for one that pruning removed
    byte missing_bytes[op_id::byte_size] = {};
    missing_bytes[0] = byte(0xAB);
    auto const missing = op_id::from_bytes(missing_bytes);

    op_id const from_missing[] = {missing};
    property_write const w[] = {{.path = p, .value = 1}};
    auto const child = add_op(graph, from_missing, w);

    auto const reachable = graph.collect_reachable(cc::span<op_id const>(&child, 1));
    CHECK(reachable.size() == 1);
    CHECK(reachable[0] == child);

    // and materialization still produces the child's own writes
    auto const doc = graph.materialize(child);
    REQUIRE(doc.try_get(p) != nullptr);
    CHECK(doc.try_get(p)->single().as_i64() == 1);

    // the child index still names the missing parent, since a child can arrive before its parent
    CHECK(graph.children(missing).size() == 1);
}

TEST("vdoc - materialize_entities equals filtering the full materialization")
{
    auto graph = op_graph();

    auto const a_path = path_of("e1", "T", "x");
    auto const b_path = path_of("e2", "T", "x");
    auto const c_path = path_of("e3", "T", "x");

    property_write const writes[]
        = {{.path = a_path, .value = 1}, {.path = b_path, .value = 2}, {.path = c_path, .value = 3}};
    auto const first = add_op(graph, {}, writes);

    op_id const from_first[] = {first};
    property_write const more[] = {{.path = a_path, .value = 10}};
    auto const second = add_op(graph, from_first, more);

    op_id const heads[] = {second};
    entity_id const wanted[] = {entity_id::of("e1"), entity_id::of("e3")};

    auto const filtered = graph.materialize_entities(heads, wanted);
    auto const full = graph.materialize(heads);

    CHECK(filtered.entities.size() == 2);
    CHECK(same_ids(writers_of(filtered, a_path), writers_of(full, a_path)));
    CHECK(same_ids(writers_of(filtered, c_path), writers_of(full, c_path)));
    CHECK(filtered.try_get(b_path) == nullptr);
    CHECK(filtered.try_get(a_path)->single().as_i64() == 10);
}

TEST("vdoc - dropping superseded sets at articulation points changes nothing")
{
    // Same DAG, both modes, compared writer for writer.
    // The fast path is what production runs, and this is what says it is only a memory optimization.
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");
    auto const q = path_of("e2", "T", "y");

    auto const root = add_op(graph, {}, {});
    op_id const from_root[] = {root};

    property_write const a_write[] = {{.path = p, .value = 1}};
    property_write const b_write[] = {{.path = p, .value = 2}, {.path = q, .value = 9}};
    auto const a = add_op(graph, from_root, a_write);
    auto const b = add_op(graph, from_root, b_write);

    op_id const ab[] = {a, b};
    property_write const x_write[] = {{.path = p, .value = 3}};
    auto const x = add_op(graph, ab, x_write);
    auto const y = add_op(graph, ab, {});

    op_id const xy[] = {x, y};
    property_write const tail_write[] = {{.path = q, .value = 10}};
    auto const tail = add_op(graph, xy, tail_write);

    op_id const heads[] = {tail};
    auto const dropped = vdoc::impl::materialize(graph, heads, {}, {.drop_superseded_at_articulation_points = true});
    auto const kept = vdoc::impl::materialize(graph, heads, {}, {.drop_superseded_at_articulation_points = false});

    CHECK(dropped.property_count() == kept.property_count());
    CHECK(same_ids(writers_of(dropped, p), writers_of(kept, p)));
    CHECK(same_ids(writers_of(dropped, q), writers_of(kept, q)));
    CHECK(same_ids(writers_of(dropped, p), oracle_writers(graph, heads, p)));
    CHECK(same_ids(writers_of(dropped, q), oracle_writers(graph, heads, q)));
}

TEST("vdoc - materialization is deterministic under shuffled insertion order")
{
    // hash(interned_string) is the precomputed hash of the bytes, so re-interning in a different sequence does not
    // move a bucket at all.
    // What can leak is INSERTION order, so that is what this shuffles.
    auto const p = path_of("e1", "T", "x");
    auto const q = path_of("e2", "T", "y");

    auto const build = [&](bool reversed)
    {
        auto graph = op_graph();

        auto const root = add_op(graph, {}, {});
        op_id const from_root[] = {root};

        property_write const a_write[] = {{.path = p, .value = 1}};
        property_write const b_write[] = {{.path = q, .value = 2}};
        auto const a = add_op(graph, from_root, a_write);
        auto const b = add_op(graph, from_root, b_write);

        // unrelated entities shift the load factor, so a bucket-order leak would show up as a different result
        for (isize i = 0; i < 32; ++i)
        {
            auto name = cc::string("filler");
            name += cc::to_string(i);
            property_write const filler[] = {{.path = path_of(name, "T", "z"), .value = i}};
            (void)add_op(graph, from_root, filler);
        }

        op_id heads[] = {a, b};
        if (reversed)
        {
            auto const tmp = heads[0];
            heads[0] = heads[1];
            heads[1] = tmp;
        }

        return graph.materialize(cc::span<op_id const>(heads, 2));
    };

    auto const forward = build(false);
    auto const backward = build(true);

    CHECK(forward.property_count() == backward.property_count());
    REQUIRE(forward.entities.size() == backward.entities.size());
    for (isize i = 0; i < forward.entities.size(); ++i)
        CHECK(forward.entities[i].entity == backward.entities[i].entity);

    CHECK(same_ids(writers_of(forward, p), writers_of(backward, p)));
    CHECK(same_ids(writers_of(forward, q), writers_of(backward, q)));
}

TEST("vdoc - re-setting an unchanged path still records a distinct writer")
{
    // Materialization has no idea what a diff is: an op that rewrites the same bytes is a later, dominating write.
    // op_builder is what avoids emitting it in the first place.
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");

    property_write const w[] = {{.path = p, .value = 5}};
    auto const a = add_op(graph, {}, w);

    op_id const from_a[] = {a};
    auto const b = add_op(graph, from_a, w);

    auto const doc = graph.materialize(b);
    CHECK(!doc.try_get(p)->is_multi_valued());
    CHECK(doc.try_get(p)->writers[0].writer == b);
}

TEST("vdoc - dropping superseded at articulation points changes nothing about the result")
{
    // The clear is an optimization with a per-SWEEP justification, and materialize_options exists so it can be turned
    // off and the two passes compared.
    // It walks a dirty list rather than every path, so a missed record would silently keep a writer the dense clear
    // would have dropped — which is exactly the difference this pins.
    auto const corpus = vdoc_test::generate_corpus();
    REQUIRE(corpus.size() > 20);

    for (auto const& c : corpus)
        for (auto const& heads : c.head_sets)
        {
            auto const cleared
                = vdoc::impl::materialize(c.graph, heads, {}, {.drop_superseded_at_articulation_points = true});
            auto const kept
                = vdoc::impl::materialize(c.graph, heads, {}, {.drop_superseded_at_articulation_points = false});

            CHECK(vdoc_test::same_document(cleared, kept));
        }
}

TEST("vdoc - a filtered sweep agrees with the oracle, cleared or not")
{
    // The filter changes which paths exist at all, so it also changes which slots the side lists cover.
    // A dense clear could not tell the two apart; a dirty list can.
    auto const corpus = vdoc_test::generate_corpus();

    for (auto const& c : corpus)
    {
        auto entities = cc::vector<entity_id>();
        for (auto const& p : c.paths)
            if (entities.empty() || !(entities.back() == p.entity))
                entities.push_back(p.entity);

        for (auto const& heads : c.head_sets)
            for (auto const& only : entities)
            {
                auto const one = cc::span<entity_id const>(&only, 1);
                auto const cleared
                    = vdoc::impl::materialize(c.graph, heads, one, {.drop_superseded_at_articulation_points = true});
                auto const kept
                    = vdoc::impl::materialize(c.graph, heads, one, {.drop_superseded_at_articulation_points = false});

                CHECK(vdoc_test::same_document(cleared, kept));

                for (auto const& path : c.paths)
                    if (path.entity == only)
                        CHECK(same_ids(writers_of(cleared, path), oracle_writers(c.graph, heads, path)));
            }
    }
}

TEST("vdoc - a discarded editing frame can be dropped outright")
{
    // The drag shape: every frame is a new op off the SAME state, and all but the last are thrown away.
    // Nothing else removes an op, so without this a ten-second drag leaves a thousand of them resident forever.
    auto graph = op_graph();

    auto const base = add_op(graph, {}, {});
    op_id const from_base[] = {base};

    auto frames = cc::vector<op_id>();
    for (isize i = 0; i < 16; ++i)
    {
        property_write const writes[] = {{.path = path_of("e1", "T", "x"), .value = i}};
        frames.push_back(add_op(graph, from_base, writes));
    }

    CHECK(graph.size() == 17);
    CHECK(graph.children(base).size() == 16);
    CHECK(graph.leaves().size() == 16);

    auto const kept = frames.back();
    for (isize i = 0; i + 1 < frames.size(); ++i)
        CHECK(graph.drop_leaf(frames[i]));

    CHECK(graph.size() == 2);
    REQUIRE(graph.children(base).size() == 1);
    CHECK(graph.children(base)[0] == kept);

    auto const leaves = graph.leaves();
    REQUIRE(leaves.size() == 1);
    CHECK(leaves[0] == kept);

    // the surviving frame materializes exactly as it did before its siblings went
    auto const p = path_of("e1", "T", "x");
    CHECK(same_ids(writers_of(graph.materialize(kept), p), oracle_writers(graph, cc::span<op_id const>(&kept, 1), p)));

    // dropping something absent is false rather than an error
    CHECK(!graph.drop_leaf(frames[0]));

    // and content addressing means a dropped frame comes back byte-identically
    property_write const again[] = {{.path = path_of("e1", "T", "x"), .value = 0}};
    CHECK(add_op(graph, from_base, again) == frames[0]);
    CHECK(graph.size() == 3);
    CHECK(graph.children(base).size() == 2);
}

TEST("vdoc - dropping the last frame leaves the base a leaf again")
{
    auto graph = op_graph();

    auto const base = add_op(graph, {}, {});
    op_id const from_base[] = {base};
    property_write const writes[] = {{.path = path_of("e1", "T", "x"), .value = 1}};
    auto const only = add_op(graph, from_base, writes);

    CHECK(graph.drop_leaf(only));
    CHECK(graph.children(base).empty());

    auto const leaves = graph.leaves();
    REQUIRE(leaves.size() == 1);
    CHECK(leaves[0] == base);
}
