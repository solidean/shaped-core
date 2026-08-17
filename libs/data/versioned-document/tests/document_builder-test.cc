#include "components.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/document_builder.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/parse.hh>

/// The builder, measured against the only oracle that means anything: a full parse.
///
/// `document` has no `operator==` and deliberately never will, so every check here compares the whole observable
/// query surface instead — which document.hh says is the intended way to compare two documents.
/// One of the component types owns heap storage, so a slot destroyed twice or never shows up under ASAN rather than
/// being invisible.

using namespace cc::primitive_defines;

using vdoc::change_kind;
using vdoc::component_registry;
using vdoc::document;
using vdoc::document_builder;
using vdoc::entity_id;
using vdoc::op_graph;

namespace
{
[[nodiscard]] component_registry test_registry()
{
    auto out = component_registry();
    out.register_component<vdoc_test::transform>();
    out.register_component<vdoc_test::mesh>();
    out.register_component<vdoc_test::tag>();
    return out;
}

/// One entity's intended contents, which the harness can both build an op from and drive the builder with.
struct wanted
{
    entity_id entity;
    cc::optional<vdoc_test::transform> transform;
    cc::optional<vdoc_test::mesh> mesh;
    bool tag = false;
};

/// The document a full parse produces for `state`, which is what the builder has to match.
[[nodiscard]] document parse_from_scratch(cc::span<wanted const> state, component_registry const& registry)
{
    auto graph = op_graph();
    auto op = vdoc::op_builder();
    for (auto const& w : state)
    {
        if (w.transform.has_value())
            op.set(w.entity, w.transform.value());
        if (w.mesh.has_value())
            op.set(w.entity, w.mesh.value());
        if (w.tag)
            op.set(w.entity, vdoc_test::tag{});

        // An entity with no component at all still has to exist, and this is how an op says so.
        if (!w.transform.has_value() && !w.mesh.has_value() && !w.tag)
            op.restore_entity(w.entity);
    }

    auto const head = graph.add(op.build(graph));
    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = vdoc::parse_report();
    return vdoc::parse(graph.materialize(head), policy, report);
}

/// Whether two documents are indistinguishable through everything a caller can ask them.
[[nodiscard]] bool same_surface(document const& a, document const& b)
{
    if (a.entity_count() != b.entity_count())
        return false;

    for (isize i = 0; i < a.entity_count(); ++i)
        if (!(a.entities()[i] == b.entities()[i]))
            return false;

    auto const types_a = a.component_types();
    auto const types_b = b.component_types();
    if (types_a.size() != types_b.size())
        return false;

    for (isize i = 0; i < types_a.size(); ++i)
        if (!(types_a[i] == types_b[i]) || a.count_of(types_a[i]) != b.count_of(types_b[i]))
            return false;

    for (auto const& e : a.entities())
    {
        auto const* const ta = a.get<vdoc_test::transform>(e);
        auto const* const tb = b.get<vdoc_test::transform>(e);
        if ((ta == nullptr) != (tb == nullptr) || (ta != nullptr && !(*ta == *tb)))
            return false;

        auto const* const ma = a.get<vdoc_test::mesh>(e);
        auto const* const mb = b.get<vdoc_test::mesh>(e);
        if ((ma == nullptr) != (mb == nullptr) || (ma != nullptr && !(*ma == *mb)))
            return false;

        if (a.has<vdoc_test::tag>(e) != b.has<vdoc_test::tag>(e))
            return false;
    }

    return true;
}

/// Sets one typed component through the builder, the way an incremental apply will.
template <class ComponentT>
change_kind set_typed(document_builder& b, component_registry const& registry, entity_id entity, ComponentT const& c)
{
    auto const* const schema = registry.try_get(vdoc::impl::component_type_of<ComponentT>());
    REQUIRE(schema != nullptr);

    return b.set_component(*schema, entity,
                           [&](byte* slot)
                           {
                               new (cc::placement_new, reinterpret_cast<ComponentT*>(slot)) ComponentT(c);
                               return true;
                           });
}

/// Drives the builder from an empty document to exactly `state`.
[[nodiscard]] document build_up(cc::span<wanted const> state, component_registry const& registry)
{
    auto b = document_builder(document());

    // The entity table must stay sorted, and insert_entity is what keeps it so whatever order they arrive in.
    for (auto const& w : state)
        CHECK(b.insert_entity(w.entity));

    for (auto const& w : state)
    {
        if (w.transform.has_value())
            CHECK(set_typed(b, registry, w.entity, w.transform.value()) == change_kind::added);
        if (w.mesh.has_value())
            CHECK(set_typed(b, registry, w.entity, w.mesh.value()) == change_kind::added);
        if (w.tag)
            CHECK(set_typed(b, registry, w.entity, vdoc_test::tag{}) == change_kind::added);
    }

    return cc::move(b).freeze();
}

/// A fixed LCG, so a failing randomized case is reproducible from its seed.
struct lcg
{
    u32 state = 1;

    [[nodiscard]] u32 next()
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }

    [[nodiscard]] u32 next_below(u32 bound) { return next() % bound; }
};
} // namespace

TEST("vdoc - a document built by the builder equals one built by a parse")
{
    auto const registry = test_registry();

    auto state = cc::vector<wanted>();
    state.push_back({.entity = entity_id::of("a"), .transform = vdoc_test::transform{.x = 1, .y = 2}});
    state.push_back({.entity = entity_id::of("b"), .mesh = vdoc_test::mesh{.asset = "wall.obj"}});
    state.push_back({.entity = entity_id::of("c"),
                     .transform = vdoc_test::transform{.x = 3, .y = 4},
                     .mesh = vdoc_test::mesh{.asset = "floor.obj"},
                     .tag = true});
    state.push_back({.entity = entity_id::of("d")});

    auto const built = build_up(state, registry);
    auto const parsed = parse_from_scratch(state, registry);

    CHECK(same_surface(built, parsed));
    CHECK(built.entity_count() == 4);
    CHECK(built.component_types().size() == 3);
}

TEST("vdoc - the builder inserts entities in sorted order whatever order they arrive")
{
    auto b = document_builder(document());

    cc::string_view const names[] = {"m", "c", "z", "a", "q", "c"};
    for (auto const& n : names)
        (void)b.insert_entity(entity_id::of(n));

    auto const doc = cc::move(b).freeze();
    REQUIRE(doc.entity_count() == 5);

    for (isize i = 1; i < doc.entity_count(); ++i)
        CHECK(std::is_lt(doc.entities()[i - 1].compare_bytes(doc.entities()[i])));

    CHECK(doc.contains(entity_id::of("a")));
    CHECK(!doc.contains(entity_id::of("b")));
}

TEST("vdoc - removing an entity takes its components with it")
{
    auto const registry = test_registry();

    auto state = cc::vector<wanted>();
    state.push_back({.entity = entity_id::of("a"), .transform = vdoc_test::transform{.x = 1}});
    state.push_back({.entity = entity_id::of("b"),
                     .transform = vdoc_test::transform{.x = 2},
                     .mesh = vdoc_test::mesh{.asset = "b.obj"}});
    state.push_back({.entity = entity_id::of("c"), .mesh = vdoc_test::mesh{.asset = "c.obj"}});

    auto b = document_builder(build_up(state, registry));
    CHECK(b.remove_entity(entity_id::of("b")));
    CHECK(!b.remove_entity(entity_id::of("b")));
    auto const doc = cc::move(b).freeze();

    state.remove_at(1);
    auto const parsed = parse_from_scratch(state, registry);

    CHECK(same_surface(doc, parsed));
    CHECK(doc.entity_count() == 2);
    CHECK(doc.get<vdoc_test::transform>(entity_id::of("b")) == nullptr);
}

TEST("vdoc - a column that loses its last component is dropped entirely")
{
    auto const registry = test_registry();

    auto state = cc::vector<wanted>();
    state.push_back({.entity = entity_id::of("a"), .transform = vdoc_test::transform{.x = 1}, .tag = true});

    auto b = document_builder(build_up(state, registry));
    CHECK(b.remove_component(vdoc::impl::component_type_of<vdoc_test::tag>(), entity_id::of("a")));

    auto const doc = cc::move(b).freeze();
    CHECK(doc.component_types().size() == 1);
    CHECK(doc.count_of(vdoc::impl::component_type_of<vdoc_test::tag>()) == 0);
    CHECK(!doc.has<vdoc_test::tag>(entity_id::of("a")));

    state[0].tag = false;
    CHECK(same_surface(doc, parse_from_scratch(state, registry)));
}

TEST("vdoc - a construct that declines removes rather than half-building")
{
    // Declining is how a parse says "drop this component", and the builder means the same by it.
    auto const registry = test_registry();
    auto const* const schema = registry.try_get(vdoc::impl::component_type_of<vdoc_test::mesh>());
    REQUIRE(schema != nullptr);

    auto state = cc::vector<wanted>();
    state.push_back({.entity = entity_id::of("a"), .mesh = vdoc_test::mesh{.asset = "a.obj"}});
    state.push_back({.entity = entity_id::of("b"), .mesh = vdoc_test::mesh{.asset = "b.obj"}});

    auto b = document_builder(build_up(state, registry));

    CHECK(b.set_component(*schema, entity_id::of("a"), [](byte*) { return false; }) == change_kind::removed);

    // and declining where there was nothing changes nothing at all
    CHECK(b.insert_entity(entity_id::of("c")));
    CHECK(b.set_component(*schema, entity_id::of("c"), [](byte*) { return false; }) == change_kind::removed);

    auto const doc = cc::move(b).freeze();
    CHECK(doc.get<vdoc_test::mesh>(entity_id::of("a")) == nullptr);
    REQUIRE(doc.get<vdoc_test::mesh>(entity_id::of("b")) != nullptr);
    CHECK(doc.get<vdoc_test::mesh>(entity_id::of("b"))->asset == "b.obj");
    CHECK(doc.count_of(vdoc::impl::component_type_of<vdoc_test::mesh>()) == 1);
}

TEST("vdoc - overwriting a component reports modified and keeps the column intact")
{
    auto const registry = test_registry();

    auto state = cc::vector<wanted>();
    for (isize i = 0; i < 8; ++i)
        state.push_back({.entity = entity_id::of(cc::format("e{}", i)),
                         .transform = vdoc_test::transform{.x = f64(i)},
                         .mesh = vdoc_test::mesh{.asset = cc::format("m{}.obj", i)}});

    auto b = document_builder(build_up(state, registry));

    CHECK(set_typed(b, registry, entity_id::of("e3"), vdoc_test::mesh{.asset = "replaced.obj"}) == change_kind::modified);
    state[3].mesh = vdoc_test::mesh{.asset = "replaced.obj"};

    auto const doc = cc::move(b).freeze();
    CHECK(same_surface(doc, parse_from_scratch(state, registry)));
}

TEST("vdoc - random batches of edits agree with a parse of the same state")
{
    // The shapes that break a dense column are insertion at the front, at the back and into the middle, plus a column
    // appearing and vanishing — a random walk hits all of them and does not need each spelled out.
    auto const registry = test_registry();
    auto rng = lcg();

    auto state = cc::vector<wanted>();
    auto doc = document();

    for (isize round = 0; round < 200; ++round)
    {
        auto b = document_builder(cc::move(doc));

        auto const name = cc::format("e{}", rng.next_below(24));
        auto const entity = entity_id::of(name);

        auto at = isize(-1);
        for (isize i = 0; i < state.size(); ++i)
            if (state[i].entity == entity)
                at = i;

        switch (rng.next_below(6))
        {
        case 0: // create or leave alone
            if (at < 0)
            {
                CHECK(b.insert_entity(entity));
                state.push_back({.entity = entity});
            }
            break;

        case 1: // set a transform
            if (at < 0)
            {
                CHECK(b.insert_entity(entity));
                state.push_back({.entity = entity});
                at = state.size() - 1;
            }
            {
                auto const t = vdoc_test::transform{.x = f64(rng.next_below(100)), .y = f64(rng.next_below(100))};
                (void)set_typed(b, registry, entity, t);
                state[at].transform = t;
            }
            break;

        case 2: // set a mesh
            if (at < 0)
            {
                CHECK(b.insert_entity(entity));
                state.push_back({.entity = entity});
                at = state.size() - 1;
            }
            {
                auto const m = vdoc_test::mesh{.asset = cc::format("asset-{}.obj", rng.next_below(1000))};
                (void)set_typed(b, registry, entity, m);
                state[at].mesh = m;
            }
            break;

        case 3: // set or clear a tag
            if (at >= 0)
            {
                if (state[at].tag)
                {
                    CHECK(b.remove_component(vdoc::impl::component_type_of<vdoc_test::tag>(), entity));
                    state[at].tag = false;
                }
                else
                {
                    (void)set_typed(b, registry, entity, vdoc_test::tag{});
                    state[at].tag = true;
                }
            }
            break;

        case 4: // drop a component
            if (at >= 0 && state[at].mesh.has_value())
            {
                CHECK(b.remove_component(vdoc::impl::component_type_of<vdoc_test::mesh>(), entity));
                state[at].mesh = {};
            }
            break;

        default: // drop the entity
            if (at >= 0)
            {
                CHECK(b.remove_entity(entity));
                state.remove_at(at);
            }
            break;
        }

        doc = cc::move(b).freeze();

        auto sorted = cc::vector<wanted>::create_copy_of(state);
        cc::sort(sorted, [](wanted const& a, wanted const& c) { return a.entity.compare_bytes(c.entity) < 0; });
        CHECK(same_surface(doc, parse_from_scratch(sorted, registry)));
    }
}

TEST("vdoc - compaction reclaims what in-place edits left behind")
{
    auto const registry = test_registry();

    auto state = cc::vector<wanted>();
    state.push_back({.entity = entity_id::of("a"), .transform = vdoc_test::transform{.x = 1}});

    auto b = document_builder(build_up(state, registry));

    // Growing a column past its capacity abandons both its arrays, so churning entities is what produces dead bytes.
    for (isize i = 0; i < 200; ++i)
    {
        auto const entity = entity_id::of(cc::format("churn-{}", i));
        CHECK(b.insert_entity(entity));
        (void)set_typed(b, registry, entity, vdoc_test::mesh{.asset = cc::format("c{}.obj", i)});
        CHECK(b.remove_entity(entity));
    }

    CHECK(b.dead_arena_bytes() > 0);

    auto const before = cc::move(b).freeze();
    auto after = document_builder(document());
    {
        auto again = document_builder(build_up(state, registry));
        for (isize i = 0; i < 200; ++i)
        {
            auto const entity = entity_id::of(cc::format("churn-{}", i));
            CHECK(again.insert_entity(entity));
            (void)set_typed(again, registry, entity, vdoc_test::mesh{.asset = cc::format("c{}.obj", i)});
            CHECK(again.remove_entity(entity));
        }

        again.compact();
        CHECK(again.dead_arena_bytes() == 0);
        after = cc::move(again);
    }

    auto const compacted = cc::move(after).freeze();

    CHECK(same_surface(before, compacted));
    CHECK(same_surface(compacted, parse_from_scratch(state, registry)));
}
