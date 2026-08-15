#include "components.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/op_graph.hh>

using namespace cc::primitive_defines;

using vdoc::component_registry;
using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::op_graph;
using vdoc::property_id;
using vdoc::property_path;

using vdoc_test::mesh;
using vdoc_test::tag;
using vdoc_test::transform;

namespace
{
/// Every path one op assigns, in the op's own canonical order.
[[nodiscard]] cc::vector<property_path> paths_of(vdoc::op const& o)
{
    auto out = cc::vector<property_path>();
    for (auto const& a : o.assignments())
        out.push_back(a.path);

    return out;
}

[[nodiscard]] bool assigns(vdoc::op const& o, cc::string_view component, cc::string_view property)
{
    for (auto const& a : o.assignments())
        if (a.path.component == component_type_id::of(component) && a.path.property == property_id::of(property))
            return true;

    return false;
}
} // namespace

TEST("vdoc - a component type registers and looks up by its interned type name")
{
    auto registry = component_registry();
    registry.register_component<transform>();
    registry.register_component<mesh>();

    CHECK(registry.size() == 2);
    CHECK(registry.contains(component_type_id::of("Transform")));
    CHECK(registry.contains(component_type_id::of("Mesh")));
    CHECK(!registry.contains(component_type_id::of("Tag")));

    auto const* const s = registry.try_get(component_type_id::of("Transform"));
    REQUIRE(s != nullptr);
    CHECK(s->current_version == 2);
    CHECK(s->component_size == isize(sizeof(transform)));
    CHECK(s->parse_into != nullptr);
    CHECK(s->destroy_range != nullptr);
    CHECK(s->write != nullptr);
}

TEST("vdoc - a registry is sorted by component type id bytes, whatever order it was filled in")
{
    auto forward = component_registry();
    forward.register_component<transform>();
    forward.register_component<mesh>();
    forward.register_component<tag>();

    auto backward = component_registry();
    backward.register_component<tag>();
    backward.register_component<mesh>();
    backward.register_component<transform>();

    REQUIRE(forward.size() == backward.size());
    for (isize i = 0; i < forward.size(); ++i)
    {
        CHECK(forward.schemas()[i].type == backward.schemas()[i].type);
        if (i > 0)
            CHECK(std::is_lt(forward.schemas()[i - 1].type.compare_bytes(forward.schemas()[i].type)));
    }
}

TEST("vdoc - registering the same type twice is idempotent")
{
    auto registry = component_registry();
    registry.register_component<transform>();
    registry.register_component<transform>();

    CHECK(registry.size() == 1);
}

TEST("vdoc - merging two registries yields the union")
{
    auto a = component_registry();
    a.register_component<transform>();

    auto b = component_registry();
    b.register_component<mesh>();
    b.register_component<transform>();

    a.merge(b);
    CHECK(a.size() == 2);
    CHECK(a.contains(component_type_id::of("Mesh")));

    // Merging is idempotent too, since every shared type carries the identical schema.
    a.merge(b);
    CHECK(a.size() == 2);
}

TEST("vdoc - reserved ids are stable across calls, and the sigil is what marks them")
{
    CHECK(vdoc::reserved::schema_version() == vdoc::reserved::schema_version());
    CHECK(vdoc::reserved::schema_version().as_string_view() == "$schema_version");
    CHECK(vdoc::reserved::alive().as_string_view() == "$alive");
    CHECK(vdoc::reserved::entity().as_string_view() == "$entity");

    CHECK(vdoc::reserved::is_reserved("$alive"));
    CHECK(!vdoc::reserved::is_reserved("alive"));
    CHECK(!vdoc::reserved::is_reserved(""));
}

TEST("vdoc - op_builder::set stamps $schema_version exactly once, alongside the component's own properties")
{
    auto const graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const op = vdoc::op_builder{}.set(e, transform{.x = 1, .y = 2}).build(graph);

    auto const paths = paths_of(op);
    CHECK(paths.size() == 3);
    CHECK(assigns(op, "Transform", "$schema_version"));
    CHECK(assigns(op, "Transform", "x"));
    CHECK(assigns(op, "Transform", "y"));

    auto stamps = isize(0);
    for (auto const& p : paths)
        if (p.property == vdoc::reserved::schema_version())
            ++stamps;
    CHECK(stamps == 1);
}

TEST("vdoc - a component with no properties still stamps its version")
{
    auto const graph = op_graph();
    auto const op = vdoc::op_builder{}.set(entity_id::of("e"), tag{}).build(graph);

    CHECK(paths_of(op).size() == 1);
    CHECK(assigns(op, "Tag", "$schema_version"));
}

TEST("vdoc - op_builder::set emits nothing when the component is unchanged")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const head = graph.add(vdoc::op_builder{}.set(e, transform{.x = 1, .y = 2}).build(graph));

    auto const same
        = vdoc::op_builder{}.set_parents(cc::span<vdoc::op_id const>(&head, 1)).set(e, transform{.x = 1, .y = 2}).build(graph);
    CHECK(paths_of(same).empty());

    // A changed field emits that field, and nothing else — the stamp already matches.
    auto const moved
        = vdoc::op_builder{}.set_parents(cc::span<vdoc::op_id const>(&head, 1)).set(e, transform{.x = 1, .y = 9}).build(graph);
    auto const moved_paths = paths_of(moved);
    REQUIRE(moved_paths.size() == 1);
    CHECK(moved_paths[0].property == property_id::of("y"));
}

TEST("vdoc - set_alive writes the reserved path, and set_entity_alive writes it on $entity")
{
    auto const graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const op
        = vdoc::op_builder{}.set_alive(e, component_type_id::of("Transform"), false).set_entity_alive(e, false).build(graph);

    CHECK(assigns(op, "Transform", "$alive"));
    CHECK(assigns(op, "$entity", "$alive"));

    for (auto const& a : op.assignments())
        CHECK(a.value.as_bool() == false);
}

TEST("vdoc - the remove and restore shorthands are exactly the set_alive calls they stand for")
{
    auto const graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const type = component_type_id::of("Transform");

    auto const removed = vdoc::op_builder{}.remove_component(e, type).remove_entity(e).build(graph);
    auto const set_false = vdoc::op_builder{}.set_alive(e, type, false).set_entity_alive(e, false).build(graph);
    CHECK(removed.id == set_false.id);

    auto const restored = vdoc::op_builder{}.restore_component(e, type).restore_entity(e).build(graph);
    auto const set_true = vdoc::op_builder{}.set_alive(e, type, true).set_entity_alive(e, true).build(graph);
    CHECK(restored.id == set_true.id);

    // Restoring something nothing removed still writes the property, because `$alive` absent already means alive.
    CHECK(paths_of(restored).size() == 2);
}

TEST("vdoc - a mesh writes and re-reads its asset through the traits")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const head = graph.add(vdoc::op_builder{}.set(e, mesh{.asset = "brick"}).build(graph));

    auto const raw = graph.materialize(head);
    auto const* const c = raw.try_get(e)->try_get(component_type_id::of("Mesh"));
    REQUIRE(c != nullptr);
    CHECK(c->try_get(property_id::of("asset"))->single().as_string() == "brick");
    CHECK(c->try_get(vdoc::reserved::schema_version())->single().as_i64() == 1);
}
