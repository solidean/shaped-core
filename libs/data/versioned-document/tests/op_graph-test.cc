#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/value_builder.hh>

#include <algorithm>

using namespace cc::primitive_defines;

using vdoc::assignment;
using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::op;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::property_id;
using vdoc::property_path;
using vdoc::raw_document;
using vdoc::value;

namespace
{
[[nodiscard]] property_path path_of(cc::string_view e, cc::string_view c, cc::string_view p)
{
    return property_path{.entity = entity_id::of(e),
                         .component = component_type_id::of(c),
                         .property = property_id::of(p)};
}

/// One write, as a test spells it: a path and an integer.
struct write
{
    property_path path;
    i64 value;
};

/// Builds and adds one op, standing in for op_builder, which does not exist yet.
///
/// The values are local because their views only have to survive as far as encode_assignments, which copies the
/// bytes into the blob — after that the op owns everything it points at.
[[nodiscard]] op_id add_op(op_graph& graph, cc::span<op_id const> parents, cc::span<write const> writes)
{
    auto sorted_parents = cc::vector<op_id>::create_copy_of(parents);
    std::sort(sorted_parents.begin(), sorted_parents.end(), op_id::by_bytes{});

    auto owned = cc::vector<value>();
    owned.reserve(writes.size());
    for (auto const& w : writes)
        owned.push_back(value::of_i64(w.value));

    auto entries = cc::vector<assignment>();
    for (isize i = 0; i < writes.size(); ++i)
        entries.push_back(assignment{.path = writes[i].path, .value = owned[i]});

    std::sort(entries.begin(), entries.end(),
              [](assignment const& a, assignment const& b) { return a.path.compare_bytes(b.path) < 0; });

    auto const metadata = vdoc::value_builder::object().build();
    auto const metadata_bytes = cc::vector<byte>::create_copy_of(metadata.bytes());
    auto const assignment_bytes = vdoc::encode_assignments(entries);

    auto const id = vdoc::compute_op_id(sorted_parents, metadata_bytes, assignment_bytes);
    auto decoded = vdoc::try_decode_op(id, sorted_parents, metadata_bytes, assignment_bytes);
    CC_ASSERT(decoded.has_value(), "the harness built an op its own decoder rejects");

    return graph.add(cc::move(decoded.value()));
}

/// The writers of one path, as sorted id bytes, for comparing two materializations.
[[nodiscard]] cc::vector<op_id> writers_of(raw_document const& doc, property_path const& path)
{
    auto out = cc::vector<op_id>();
    auto const* const p = doc.try_get(path);
    if (p == nullptr)
        return out;

    for (auto const& w : p->writers)
        out.push_back(w.writer);

    return out;
}

[[nodiscard]] bool same_ids(cc::span<op_id const> a, cc::span<op_id const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}

/// The deliberately stupid reference: a writer survives unless some OTHER writer of the same path descends from it.
///
/// This is what the real pass is checked against.
/// It is exponentially worse and obviously correct, which is the point: milestone 6 checks its snapshot cache
/// against the real pass, so the real pass needs an oracle of its own.
[[nodiscard]] cc::vector<op_id> oracle_writers(op_graph const& graph, cc::span<op_id const> heads, property_path const& path)
{
    auto const reachable = graph.collect_reachable(heads);

    auto writers = cc::vector<op_id>();
    for (auto const& id : reachable)
    {
        auto const* const o = graph.find(id);
        for (auto const a : o->assignments())
            if (a.path == path)
                writers.push_back(id);
    }

    auto const descends_from = [&](op_id const& from, op_id const& ancestor)
    {
        auto stack = cc::vector<op_id>();
        stack.push_back(from);
        auto seen = cc::vector<op_id>();
        while (!stack.empty())
        {
            auto const cur = stack.back();
            stack.remove_back();

            auto const* const o = graph.find(cur);
            if (o == nullptr)
                continue;

            for (auto const& p : o->parents)
            {
                if (p == ancestor)
                    return true;

                auto already = false;
                for (auto const& s : seen)
                    already = already || s == p;
                if (already)
                    continue;

                seen.push_back(p);
                stack.push_back(p);
            }
        }
        return false;
    };

    auto out = cc::vector<op_id>();
    for (auto const& w : writers)
    {
        auto dominated = false;
        for (auto const& other : writers)
            dominated = dominated || (!(other == w) && descends_from(other, w));

        if (!dominated)
            out.push_back(w);
    }

    std::sort(out.begin(), out.end(), op_id::by_bytes{});
    return out;
}
} // namespace

TEST("vdoc - add is idempotent and does not disturb the child index")
{
    auto graph = op_graph();

    auto const root = add_op(graph, {}, {});
    op_id const parents[] = {root};
    write const writes[] = {{.path = path_of("e1", "T", "x"), .value = 1}};
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

    write const first[] = {{.path = p, .value = 1}};
    auto const a = add_op(graph, {}, first);

    op_id const from_a[] = {a};
    write const second[] = {{.path = p, .value = 2}};
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

    write const left_write[] = {{.path = p, .value = 1}};
    write const right_write[] = {{.path = p, .value = 2}};
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
    write const same[] = {{.path = p, .value = 7}};
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

    write const a_write[] = {{.path = p, .value = 1}};
    write const b_write[] = {{.path = p, .value = 2}};
    auto const a = add_op(graph, from_root, a_write);
    auto const b = add_op(graph, from_root, b_write);

    op_id const ab[] = {a, b};
    write const x_write[] = {{.path = p, .value = 3}};
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

    write const first[] = {{.path = p, .value = 1}};
    auto const a = add_op(graph, {}, first);

    op_id const from_a[] = {a};
    write const second[] = {{.path = p, .value = 2}};
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

    write const left_write[] = {{.path = left_path, .value = 1}};
    write const right_write[] = {{.path = right_path, .value = 2}};
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
    write const w[] = {{.path = p, .value = 1}};
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

    write const writes[] = {{.path = a_path, .value = 1}, {.path = b_path, .value = 2}, {.path = c_path, .value = 3}};
    auto const first = add_op(graph, {}, writes);

    op_id const from_first[] = {first};
    write const more[] = {{.path = a_path, .value = 10}};
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

    write const a_write[] = {{.path = p, .value = 1}};
    write const b_write[] = {{.path = p, .value = 2}, {.path = q, .value = 9}};
    auto const a = add_op(graph, from_root, a_write);
    auto const b = add_op(graph, from_root, b_write);

    op_id const ab[] = {a, b};
    write const x_write[] = {{.path = p, .value = 3}};
    auto const x = add_op(graph, ab, x_write);
    auto const y = add_op(graph, ab, {});

    op_id const xy[] = {x, y};
    write const tail_write[] = {{.path = q, .value = 10}};
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

        write const a_write[] = {{.path = p, .value = 1}};
        write const b_write[] = {{.path = q, .value = 2}};
        auto const a = add_op(graph, from_root, a_write);
        auto const b = add_op(graph, from_root, b_write);

        // unrelated entities shift the load factor, so a bucket-order leak would show up as a different result
        for (isize i = 0; i < 32; ++i)
        {
            auto name = cc::string("filler");
            name += cc::to_string(i);
            write const filler[] = {{.path = path_of(name, "T", "z"), .value = i}};
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

    write const w[] = {{.path = p, .value = 5}};
    auto const a = add_op(graph, {}, w);

    op_id const from_a[] = {a};
    auto const b = add_op(graph, from_a, w);

    auto const doc = graph.materialize(b);
    CHECK(!doc.try_get(p)->is_multi_valued());
    CHECK(doc.try_get(p)->writers[0].writer == b);
}
