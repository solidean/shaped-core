#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/value_builder.hh>

using namespace cc::primitive_defines;

using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::op_builder;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::property_id;
using vdoc::property_path;
using vdoc::value;

namespace
{
[[nodiscard]] property_path path_of(cc::string_view e, cc::string_view c, cc::string_view p)
{
    return property_path{.entity = entity_id::of(e),
                         .component = component_type_id::of(c),
                         .property = property_id::of(p)};
}

[[nodiscard]] isize assignment_count(vdoc::op const& o)
{
    return o.assignments().remaining();
}
} // namespace

TEST("vdoc - a first op writes everything it was given")
{
    auto graph = op_graph();

    auto const o = op_builder()
                       .set_raw(path_of("e1", "T", "x"), value::of(1))
                       .set_raw(path_of("e1", "T", "y"), value::of(2))
                       .build(graph);

    CHECK(assignment_count(o) == 2);
    CHECK(o.parents.empty());
    CHECK(vdoc::verify_op(o) == vdoc::op_verification::verified);
}

TEST("vdoc - re-setting an unchanged property produces an op with no assignments")
{
    auto graph = op_graph();

    auto const p = path_of("e1", "T", "x");
    auto const q = path_of("e1", "T", "y");

    auto const first = graph.add(op_builder().set_raw(p, value::of(1)).set_raw(q, value::of(2)).build(graph));
    op_id const head[] = {first};

    // the same values again: nothing changed, so there is nothing to write down
    auto const unchanged = op_builder().set_parents(head).set_raw(p, value::of(1)).set_raw(q, value::of(2)).build(graph);
    CHECK(assignment_count(unchanged) == 0);

    // one field differs: exactly one assignment, not two
    auto const changed = op_builder().set_parents(head).set_raw(p, value::of(1)).set_raw(q, value::of(99)).build(graph);
    CHECK(assignment_count(changed) == 1);
    CHECK(changed.assignments().get().path == q);
    CHECK(changed.assignments().get().value.as_i64() == 99);
}

TEST("vdoc - a multi-valued property always differs, even when the writers agree")
{
    auto graph = op_graph();
    auto const p = path_of("e1", "T", "x");

    auto const root = graph.add(op_builder().build(graph));
    op_id const from_root[] = {root};

    // two concurrent writers of the SAME bytes: structurally multi-valued, whatever the values say
    auto const left = graph.add(op_builder().set_parents(from_root).set_raw(p, value::of(7)).build(graph));
    auto const spacer
        = graph.add(op_builder().set_parents(from_root).set_raw(path_of("e2", "T", "z"), value::of(0)).build(graph));
    op_id const from_spacer[] = {spacer};
    auto const right = graph.add(op_builder().set_parents(from_spacer).set_raw(p, value::of(7)).build(graph));

    op_id const heads[] = {left, right};
    REQUIRE(graph.materialize(heads).try_get(p)->is_multi_valued());

    // setting the value the user already sees is how a conflict is resolved, so it must emit
    auto const resolving = op_builder().set_parents(heads).set_raw(p, value::of(7)).build(graph);
    CHECK(assignment_count(resolving) == 1);

    // and once it has, the path is single-valued again, so a second build emits nothing
    auto const resolved = graph.add(cc::move(resolving));
    op_id const after[] = {resolved};
    CHECK(!graph.materialize(after).try_get(p)->is_multi_valued());
    CHECK(assignment_count(op_builder().set_parents(after).set_raw(p, value::of(7)).build(graph)) == 0);
}

TEST("vdoc - identical content produces one id whatever order the caller supplied")
{
    auto graph = op_graph();

    auto const a = path_of("e1", "T", "x");
    auto const b = path_of("e2", "T", "y");
    auto const c = path_of("e1", "T", "a");

    auto const forward
        = op_builder().set_raw(a, value::of(1)).set_raw(b, value::of(2)).set_raw(c, value::of(3)).build(graph);
    auto const backward
        = op_builder().set_raw(c, value::of(3)).set_raw(b, value::of(2)).set_raw(a, value::of(1)).build(graph);

    CHECK(forward.id == backward.id);
}

TEST("vdoc - parents are sorted and deduplicated before they reach the hash")
{
    auto graph = op_graph();

    auto const first = graph.add(op_builder().set_raw(path_of("e1", "T", "x"), value::of(1)).build(graph));
    auto const second = graph.add(op_builder().set_raw(path_of("e1", "T", "y"), value::of(2)).build(graph));

    op_id const forward[] = {first, second};
    op_id const backward[] = {second, first};
    op_id const with_duplicate[] = {first, second, first};

    auto const a = op_builder().set_parents(forward).set_raw(path_of("e2", "T", "z"), value::of(3)).build(graph);
    auto const b = op_builder().set_parents(backward).set_raw(path_of("e2", "T", "z"), value::of(3)).build(graph);
    auto const c = op_builder().set_parents(with_duplicate).set_raw(path_of("e2", "T", "z"), value::of(3)).build(graph);

    CHECK(a.id == b.id);
    CHECK(a.id == c.id);
    CHECK(a.parents.size() == 2);

    // and the stored order is the canonical one, since that is what was hashed
    CHECK(std::is_lt(a.parents[0].compare_bytes(a.parents[1])));
}

TEST("vdoc - metadata is hashed but does not change what is written")
{
    auto graph = op_graph();
    auto const p = path_of("e1", "T", "x");

    auto const plain = op_builder().set_raw(p, value::of(1)).build(graph);
    auto const annotated = op_builder()
                               .set_metadata(vdoc::value_builder::object().set("author", "pt").build())
                               .set_raw(p, value::of(1))
                               .build(graph);

    // same assignments, different op: metadata cannot be altered after the fact because the id commits to it
    CHECK(assignment_count(plain) == assignment_count(annotated));
    CHECK(!(plain.id == annotated.id));
    CHECK(annotated.metadata().kind() == vdoc::value_kind::object);
}

TEST("vdoc - the diff only materializes the entities it touches")
{
    auto graph = op_graph();

    auto builder = op_builder();
    for (isize i = 0; i < 16; ++i)
    {
        auto name = cc::string("e");
        name += cc::to_string(i);
        builder.set_raw(path_of(name, "T", "x"), value::of(i));
    }
    auto const wide = graph.add(builder.build(graph));
    op_id const head[] = {wide};

    // touching one entity of sixteen still sees that entity's current value, and emits nothing when it matches
    CHECK(assignment_count(op_builder().set_parents(head).set_raw(path_of("e7", "T", "x"), value::of(7)).build(graph))
          == 0);
    CHECK(assignment_count(op_builder().set_parents(head).set_raw(path_of("e7", "T", "x"), value::of(8)).build(graph))
          == 1);

    // and a path under an untouched entity is still absent as far as this op is concerned, so it writes
    CHECK(assignment_count(op_builder().set_parents(head).set_raw(path_of("new", "T", "x"), value::of(0)).build(graph))
          == 1);
}

TEST("vdoc - staging the same path twice is a caller bug")
{
    auto graph = op_graph();
    auto const p = path_of("e1", "T", "x");

    CHECK_ASSERTS(op_builder().set_raw(p, value::of(1)).set_raw(p, value::of(2)));
    (void)graph;
}
