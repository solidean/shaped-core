#include "components.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document/document.hh>

#include <algorithm>

using namespace cc::primitive_defines;

using vdoc::component_type_id;
using vdoc::document;
using vdoc::entity_id;

using vdoc_test::mesh;
using vdoc_test::tag;
using vdoc_test::transform;

namespace
{
/// One component of one type, as a test spells it out.
template <class ComponentT>
struct placement
{
    entity_id entity;
    ComponentT component;
};

/// Builds a document straight through the parser seam, with no raw document behind it.
///
/// Every list must already be sorted by entity id bytes, and the columns are added in the type order the parser walks.
struct document_builder
{
    vdoc::impl::parser p;
    cc::vector<entity_id> entities;

    template <class ComponentT>
    void add_column(cc::span<placement<ComponentT> const> items)
    {
        if (items.empty())
            return;

        p.begin_column(vdoc::impl::make_component_schema<ComponentT>(), items.size());
        for (auto const& item : items)
            p.push_component(item.entity,
                             [&](byte* slot)
                             {
                                 new (cc::placement_new, reinterpret_cast<ComponentT*>(slot)) ComponentT(item.component);
                                 return true;
                             });
        p.end_column();
    }

    [[nodiscard]] document finish()
    {
        p.set_entities(entities);
        return p.finish();
    }
};

[[nodiscard]] cc::vector<entity_id> ids_of(cc::span<cc::string_view const> names)
{
    auto out = cc::vector<entity_id>();
    for (auto const& n : names)
        out.push_back(entity_id::of(n));

    std::sort(out.begin(), out.end(), entity_id::by_bytes{});
    return out;
}

/// The naive intersection of two sorted id lists, for comparing against the leapfrog join.
[[nodiscard]] cc::vector<entity_id> intersect(cc::span<entity_id const> a, cc::span<entity_id const> b)
{
    auto out = cc::vector<entity_id>();
    for (auto const& x : a)
        for (auto const& y : b)
            if (x == y)
                out.push_back(x);

    return out;
}
} // namespace

TEST("vdoc - an empty document has no entities, no columns and no components")
{
    auto const doc = document();

    CHECK(doc.entity_count() == 0);
    CHECK(doc.entities().empty());
    CHECK(doc.component_types().empty());
    CHECK(!doc.contains(entity_id::of("a")));
    CHECK(doc.get<transform>(entity_id::of("a")) == nullptr);
    CHECK(!doc.has<transform>(entity_id::of("a")));
    CHECK(doc.count_of(component_type_id::of("Transform")) == 0);

    auto visits = isize(0);
    doc.each<transform>([&](entity_id, transform const&) { ++visits; });
    doc.each<transform, mesh>([&](entity_id, transform const&, mesh const&) { ++visits; });
    CHECK(visits == 0);
}

TEST("vdoc - get finds every component a linear scan finds, and nothing else")
{
    cc::string_view const names[] = {"a", "b", "c", "d"};
    auto b = document_builder();
    b.entities = ids_of(names);

    placement<transform> const transforms[]
        = {{entity_id::of("a"), {.x = 1, .y = 1}}, {entity_id::of("c"), {.x = 3, .y = 3}}};
    b.add_column<transform>(transforms);

    auto const doc = b.finish();

    CHECK(doc.entity_count() == 4);
    CHECK(doc.count_of(component_type_id::of("Transform")) == 2);

    for (auto const& e : doc.entities())
    {
        auto const* const found = doc.get<transform>(e);

        transform const* expected = nullptr;
        for (auto const& t : transforms)
            if (t.entity == e)
                expected = &t.component;

        CHECK((found == nullptr) == (expected == nullptr));
        if (expected != nullptr)
            CHECK(*found == *expected);
    }

    CHECK(doc.get<transform>(entity_id::of("nothing-like-this")) == nullptr);
}

TEST("vdoc - each over one type visits every element in ascending entity id order")
{
    cc::string_view const names[] = {"a", "b", "c"};
    auto b = document_builder();
    b.entities = ids_of(names);

    placement<transform> const transforms[]
        = {{entity_id::of("a"), {.x = 1}}, {entity_id::of("b"), {.x = 2}}, {entity_id::of("c"), {.x = 3}}};
    b.add_column<transform>(transforms);

    auto const doc = b.finish();

    auto seen = cc::vector<entity_id>();
    auto sum = 0.0;
    doc.each<transform>(
        [&](entity_id e, transform const& t)
        {
            seen.push_back(e);
            sum += t.x;
        });

    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == entity_id::of("a"));
    CHECK(seen[1] == entity_id::of("b"));
    CHECK(seen[2] == entity_id::of("c"));
    CHECK(sum == 6.0);
}

TEST("vdoc - a two-type join equals the naively computed intersection")
{
    cc::string_view const names[] = {"a", "b", "c", "d", "e"};
    auto b = document_builder();
    b.entities = ids_of(names);

    placement<mesh> const meshes[] = {{entity_id::of("a"), {.asset = "x"}},
                                      {entity_id::of("c"), {.asset = "y"}},
                                      {entity_id::of("e"), {.asset = "z"}}};
    placement<transform> const transforms[] = {{entity_id::of("b"), {.x = 2}},
                                               {entity_id::of("c"), {.x = 3}},
                                               {entity_id::of("d"), {.x = 4}},
                                               {entity_id::of("e"), {.x = 5}}};

    // Columns go in ascending component type id order: "Mesh" before "Transform".
    b.add_column<mesh>(meshes);
    b.add_column<transform>(transforms);

    auto const doc = b.finish();

    auto mesh_ids = cc::vector<entity_id>();
    for (auto const& m : meshes)
        mesh_ids.push_back(m.entity);
    auto transform_ids = cc::vector<entity_id>();
    for (auto const& t : transforms)
        transform_ids.push_back(t.entity);

    auto const expected = intersect(mesh_ids, transform_ids);

    auto joined = cc::vector<entity_id>();
    doc.each<mesh, transform>(
        [&](entity_id e, mesh const& m, transform const& t)
        {
            joined.push_back(e);
            CHECK(doc.get<mesh>(e)->asset == m.asset);
            CHECK(doc.get<transform>(e)->x == t.x);
        });

    REQUIRE(joined.size() == expected.size());
    for (isize i = 0; i < joined.size(); ++i)
        CHECK(joined[i] == expected[i]);
}

TEST("vdoc - a three-type join equals the naively computed intersection")
{
    cc::string_view const names[] = {"a", "b", "c", "d", "e", "f", "g"};
    auto b = document_builder();
    b.entities = ids_of(names);

    placement<mesh> const meshes[] = {{entity_id::of("a"), {.asset = "x"}},
                                      {entity_id::of("c"), {.asset = "y"}},
                                      {entity_id::of("d"), {.asset = "z"}},
                                      {entity_id::of("g"), {.asset = "w"}}};
    placement<tag> const tags[]
        = {{entity_id::of("b"), {}}, {entity_id::of("c"), {}}, {entity_id::of("d"), {}}, {entity_id::of("g"), {}}};
    placement<transform> const transforms[]
        = {{entity_id::of("c"), {.x = 3}}, {entity_id::of("e"), {.x = 5}}, {entity_id::of("g"), {.x = 7}}};

    b.add_column<mesh>(meshes);
    b.add_column<tag>(tags);
    b.add_column<transform>(transforms);

    auto const doc = b.finish();

    auto joined = cc::vector<entity_id>();
    doc.each<mesh, tag, transform>([&](entity_id e, mesh const&, tag const&, transform const&) { joined.push_back(e); });

    // c and g carry all three.
    REQUIRE(joined.size() == 2);
    CHECK(joined[0] == entity_id::of("c"));
    CHECK(joined[1] == entity_id::of("g"));
}

TEST("vdoc - a join against a missing or single-element column degenerates correctly")
{
    cc::string_view const names[] = {"a", "b"};
    auto b = document_builder();
    b.entities = ids_of(names);

    placement<transform> const transforms[] = {{entity_id::of("a"), {.x = 1}}};
    b.add_column<transform>(transforms);

    auto const doc = b.finish();

    auto visits = isize(0);
    doc.each<transform>([&](entity_id, transform const&) { ++visits; });
    CHECK(visits == 1);

    // Mesh has no column at all, so the join is empty rather than a crash.
    visits = 0;
    doc.each<transform, mesh>([&](entity_id, transform const&, mesh const&) { ++visits; });
    CHECK(visits == 0);
}

TEST("vdoc - a column that keeps nothing is not a column")
{
    auto b = document_builder();
    cc::string_view const names[] = {"a"};
    b.entities = ids_of(names);

    b.p.begin_column(vdoc::impl::make_component_schema<transform>(), 4);
    CHECK(!b.p.push_component(entity_id::of("a"), [](byte*) { return false; }));
    b.p.end_column();

    auto const doc = b.finish();
    CHECK(doc.component_types().empty());
    CHECK(doc.count_of(component_type_id::of("Transform")) == 0);
    CHECK(doc.entity_count() == 1);
}

TEST("vdoc - an entity with no known component is still in the entity list")
{
    cc::string_view const names[] = {"a", "b"};
    auto b = document_builder();
    b.entities = ids_of(names);

    placement<transform> const transforms[] = {{entity_id::of("a"), {.x = 1}}};
    b.add_column<transform>(transforms);

    auto const doc = b.finish();
    CHECK(doc.contains(entity_id::of("b")));
    CHECK(!doc.has<transform>(entity_id::of("b")));
}

TEST("vdoc - a moved document keeps its components alive, and the moved-from one is empty")
{
    cc::string_view const names[] = {"a", "b"};
    auto b = document_builder();
    b.entities = ids_of(names);

    // Heap-owning components, so a double release or a use-after-free is an actual fault rather than stale bytes.
    placement<mesh> const meshes[]
        = {{entity_id::of("a"), {.asset = "a-very-long-asset-name-that-will-not-fit-inline"}},
           {entity_id::of("b"), {.asset = "another-very-long-asset-name-that-will-not-fit-inline"}}};
    b.add_column<mesh>(meshes);

    auto source = b.finish();
    auto moved = cc::move(source);

    CHECK(source.entity_count() == 0);
    CHECK(source.component_types().empty());

    REQUIRE(moved.entity_count() == 2);
    CHECK(moved.get<mesh>(entity_id::of("a"))->asset == "a-very-long-asset-name-that-will-not-fit-inline");

    auto assigned = document();
    assigned = cc::move(moved);
    CHECK(assigned.get<mesh>(entity_id::of("b"))->asset == "another-very-long-asset-name-that-will-not-fit-inline");
}

TEST("vdoc - a document holds far more components than one arena block")
{
    auto names = cc::vector<cc::string>();
    for (isize i = 0; i < 4000; ++i)
        names.push_back(cc::to_string(10000 + i));

    auto views = cc::vector<cc::string_view>();
    for (auto const& n : names)
        views.push_back(n);

    auto b = document_builder();
    b.entities = ids_of(views);

    auto items = cc::vector<placement<transform>>();
    for (auto const& e : b.entities)
        items.push_back({e, transform{.x = f64(items.size())}});

    b.add_column<transform>(items);
    auto const doc = b.finish();

    CHECK(doc.count_of(component_type_id::of("Transform")) == 4000);

    auto visits = isize(0);
    doc.each<transform>(
        [&](entity_id e, transform const& t)
        {
            CHECK(doc.get<transform>(e)->x == t.x);
            ++visits;
        });
    CHECK(visits == 4000);
}
