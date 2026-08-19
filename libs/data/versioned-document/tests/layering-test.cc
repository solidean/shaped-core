#include "components.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/layer_stack.hh>
#include <versioned-document/op_builder.hh>

/// Layering, measured against the one thing it is defined to equal.
///
/// **The oracle is a flattening.** "A higher layer replaces a lower one per property path" is exactly "written later on
/// a single-parent chain", so replaying the layers bottom-to-top into one graph and parsing that must give the same
/// document the stack composes.
/// It holds only where each layer is single-valued, which is what a flattening can express — a multi-value inside a
/// layer survives composition untouched and has its own test.
///
/// See ../docs/concepts/layering.md.

using namespace cc::primitive_defines;

using vdoc::change_summary;
using vdoc::component_registry;
using vdoc::component_type_id;
using vdoc::direct_layer;
using vdoc::document;
using vdoc::entity_id;
using vdoc::layer_stack;
using vdoc::op_builder;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::parse_report;
using vdoc::property_id;
using vdoc::property_path;
using vdoc::value;

using namespace vdoc_test;

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

[[nodiscard]] property_path path_of(cc::string_view e, cc::string_view c, cc::string_view p)
{
    return property_path{.entity = entity_id::of(e),
                         .component = component_type_id::of(c),
                         .property = property_id::of(p)};
}

/// The version stamp `op_builder::set` would have written.
///
/// A layer that contributes a whole component owes it, and a layer that overrides one property deliberately does not —
/// so the composed `$schema_version` comes from whoever supplied the component, which is the rule the docs state.
/// Without it `transform` reads as version 0 and takes its migration path, which is a different test.
void stamp_transform(direct_layer& layer, cc::string_view e)
{
    layer.set(path_of(e, "Transform", "$schema_version"), value::of(i64(2)));
}

void stamp_transform(op_builder& staged, cc::string_view e)
{
    staged.set_raw(path_of(e, "Transform", "$schema_version"), value::of(i64(2)));
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
        for (auto const& t : types_a)
            if (a.has_component(t, e) != b.has_component(t, e))
                return false;

        auto const* const ta = a.get<vdoc_test::transform>(e);
        auto const* const tb = b.get<vdoc_test::transform>(e);
        if ((ta == nullptr) != (tb == nullptr) || (ta != nullptr && !(*ta == *tb)))
            return false;

        auto const* const ma = a.get<vdoc_test::mesh>(e);
        auto const* const mb = b.get<vdoc_test::mesh>(e);
        if ((ma == nullptr) != (mb == nullptr) || (ma != nullptr && !(*ma == *mb)))
            return false;
    }

    return true;
}

/// Replays every layer's surviving writes bottom-to-top into one single-parent chain.
///
/// This is the oracle: one history whose later ops dominate is the unlayered spelling of "a higher layer wins per path".
[[nodiscard]] op_id flatten(op_graph& out, cc::span<vdoc::raw_document const* const> layers)
{
    auto head = cc::vector<op_id>();

    for (auto const* const doc : layers)
    {
        auto staged = op_builder();
        staged.set_parents(head);

        for (auto const& e : doc->entities)
            for (auto const& c : e.value.components)
                for (auto const& p : c.value.properties)
                {
                    REQUIRE(!p.value.is_multi_valued());
                    staged.set_raw({.entity = e.entity, .component = c.component, .property = p.property},
                                   value::from_validated_bytes(p.value.single().bytes()));
                }

        auto const id = out.add(staged.build(out));
        head = cc::vector<op_id>{id};
    }

    return head.empty() ? op_id() : head[0];
}

/// The document a flattening of these layers parses to.
[[nodiscard]] document flattened_document(cc::span<vdoc::raw_document const* const> layers,
                                          vdoc::parse_policy const& policy)
{
    auto graph = op_graph();
    auto const head = flatten(graph, layers);

    auto report = parse_report();
    return vdoc::parse(graph.materialize(head), policy, report);
}
} // namespace

TEST("vdoc - a higher layer overrides one property and leaves its siblings alone")
{
    // The test that names the feature.
    // Component-granular replacement would freeze `y` at the moment the override was made, which is exactly the
    // rebasing layering exists for, broken.
    auto const registry = test_registry();
    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto changes = change_summary();

    auto base = direct_layer("base");
    stamp_transform(base, "e1");
    base.set(path_of("e1", "Transform", "x"), value::of(1.0));
    base.set(path_of("e1", "Transform", "y"), value::of(2.0));

    auto graph = op_graph();
    auto const override_head
        = graph.add(op_builder().set_raw(path_of("e1", "Transform", "x"), value::of(99.0)).build(graph));

    auto stack = layer_stack();
    (void)stack.push_direct_layer("base", base);
    (void)stack.push_graph_layer("user", graph, override_head);

    stack.rebuild(policy, report, changes);

    auto const* const t = stack.composed().get<vdoc_test::transform>(entity_id::of("e1"));
    REQUIRE(t != nullptr);
    CHECK(t->x == 99.0); // the override took
    CHECK(t->y == 2.0);  // and the sibling came from below, unfrozen

    // now the base animates y, and the override must still hold x while y follows
    base.set(path_of("e1", "Transform", "y"), value::of(7.0));

    auto stats = vdoc::layered_apply_stats();
    stack.apply(policy, report, changes, {}, &stats);
    CHECK(stats.took_fast_path);

    auto const* const moved = stack.composed().get<vdoc_test::transform>(entity_id::of("e1"));
    REQUIRE(moved != nullptr);
    CHECK(moved->x == 99.0);
    CHECK(moved->y == 7.0);
}

TEST("vdoc - abstaining in a higher layer reveals the layer below")
{
    auto const registry = test_registry();
    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto changes = change_summary();

    auto base = direct_layer("base");
    stamp_transform(base, "e1");
    base.set(path_of("e1", "Transform", "x"), value::of(1.0));
    base.set(path_of("e1", "Transform", "y"), value::of(2.0));

    auto graph = op_graph();
    auto const overridden
        = graph.add(op_builder().set_raw(path_of("e1", "Transform", "x"), value::of(99.0)).build(graph));

    auto stack = layer_stack();
    (void)stack.push_direct_layer("base", base);
    auto const user = stack.push_graph_layer("user", graph, overridden);
    stack.rebuild(policy, report, changes);

    REQUIRE(stack.composed().get<vdoc_test::transform>(entity_id::of("e1"))->x == 99.0);

    // withdrawing the override reverts to the base rather than to zero or to absent
    op_id const from_overridden[] = {overridden};
    auto const withdrawn
        = graph.add(op_builder().set_parents(from_overridden).abstain(path_of("e1", "Transform", "x")).build(graph));

    stack.set_head(user, withdrawn);

    auto stats = vdoc::layered_apply_stats();
    stack.apply(policy, report, changes, {}, &stats);
    CHECK(stats.took_fast_path);

    auto const* const reverted = stack.composed().get<vdoc_test::transform>(entity_id::of("e1"));
    REQUIRE(reverted != nullptr);
    CHECK(reverted->x == 1.0); // the base's value, not a default
    CHECK(reverted->y == 2.0);

    // and provenance says so: x now comes from the base again
    CHECK(stack.provenance_of(path_of("e1", "Transform", "x")) == stack.layer_at(0));
}

TEST("vdoc - a composed stack equals a flattening of its layers")
{
    auto const registry = test_registry();
    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto changes = change_summary();

    // Three layers over overlapping entities, so every interesting case is present: a path only one layer has, a path
    // two layers disagree about, and an entity only the top layer introduces.
    auto low = op_graph();
    auto low_staged = op_builder();
    stamp_transform(low_staged, "e1");
    auto const low_head = low.add(low_staged.set_raw(path_of("e1", "Transform", "x"), value::of(1.0))
                                      .set_raw(path_of("e1", "Transform", "y"), value::of(2.0))
                                      .set_raw(path_of("e2", "Mesh", "asset"), value::of("low.obj"))
                                      .build(low));

    auto mid = op_graph();
    auto const mid_head = mid.add(op_builder()
                                      .set_raw(path_of("e1", "Transform", "x"), value::of(10.0))
                                      .set_raw(path_of("e2", "Mesh", "asset"), value::of("mid.obj"))
                                      .build(mid));

    auto high = op_graph();
    auto high_staged = op_builder();
    stamp_transform(high_staged, "e3"); // e3 exists only here, so this layer is the one that owes its version
    auto const high_head = high.add(high_staged.set_raw(path_of("e2", "Mesh", "asset"), value::of("high.obj"))
                                        .set_raw(path_of("e3", "Transform", "x"), value::of(3.0))
                                        .build(high));

    auto stack = layer_stack();
    (void)stack.push_graph_layer("low", low, low_head);
    (void)stack.push_graph_layer("mid", mid, mid_head);
    (void)stack.push_graph_layer("high", high, high_head);
    stack.rebuild(policy, report, changes);

    auto const low_raw = low.materialize(low_head);
    auto const mid_raw = mid.materialize(mid_head);
    auto const high_raw = high.materialize(high_head);
    vdoc::raw_document const* const layers[] = {&low_raw, &mid_raw, &high_raw};

    CHECK(same_surface(stack.composed(), flattened_document(layers, policy)));

    // spelled out too, so a failure says which rule broke rather than only that two documents differ
    auto const* const t = stack.composed().get<vdoc_test::transform>(entity_id::of("e1"));
    REQUIRE(t != nullptr);
    CHECK(t->x == 10.0); // mid wins over low
    CHECK(t->y == 2.0);  // low is the only one that has it

    auto const* const m = stack.composed().get<vdoc_test::mesh>(entity_id::of("e2"));
    REQUIRE(m != nullptr);
    CHECK(m->asset == "high.obj"); // the top of three
    CHECK(stack.composed().contains(entity_id::of("e3")));
}

TEST("vdoc - an incremental layered apply agrees with a rebuild at every step")
{
    // The differential that matters: every edit is applied incrementally to one stack and rebuilt on another, and the
    // two must never diverge.
    auto const registry = test_registry();
    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);

    auto base = direct_layer("base");
    auto user = op_graph();
    auto forced = direct_layer("forced");

    // The base owns the components, so it is the layer that stamps their versions.
    for (isize i = 0; i < 5; ++i)
        stamp_transform(base, cc::format("e{}", i));

    auto staged_seed = op_builder();
    auto head = user.add(staged_seed.set_raw(path_of("e0", "Transform", "x"), value::of(0.0)).build(user));

    auto fast = layer_stack();
    (void)fast.push_direct_layer("base", base);
    auto const fast_user = fast.push_graph_layer("user", user, head);
    (void)fast.push_direct_layer("forced", forced);

    auto fast_report = parse_report();
    auto fast_changes = change_summary();
    fast.rebuild(policy, fast_report, fast_changes);

    auto ever_fast = false;

    for (isize round = 0; round < 24; ++round)
    {
        // rotate through the three layers, so each one drives an apply
        switch (round % 3)
        {
        case 0:
        {
            auto const e = cc::format("e{}", round % 5);
            base.set(path_of(e, "Transform", "x"), value::of(f64(round)));
            base.set(path_of(e, "Transform", "y"), value::of(f64(round) * 2));
            break;
        }
        case 1:
        {
            op_id const from[] = {head};
            auto staged = op_builder();
            staged.set_parents(from);
            staged.set_raw(path_of(cc::format("e{}", round % 5), "Transform", "x"), value::of(f64(round) * 10));
            head = user.add(staged.build(user));
            fast.set_head(fast_user, head);
            break;
        }
        default:
        {
            auto const e = cc::format("e{}", round % 5);
            if (round % 6 == 2)
                forced.set(path_of(e, "Transform", "x"), value::of(-1.0));
            else
                forced.abstain(path_of(e, "Transform", "x"));
            break;
        }
        }

        auto stats = vdoc::layered_apply_stats();
        fast.apply(policy, fast_report, fast_changes, {}, &stats);
        ever_fast = ever_fast || stats.took_fast_path;

        // a second stack over the same layers, composed from nothing
        auto slow = layer_stack();
        (void)slow.push_direct_layer("base", base);
        (void)slow.push_graph_layer("user", user, head);
        (void)slow.push_direct_layer("forced", forced);

        auto slow_report = parse_report();
        auto slow_changes = change_summary();
        slow.rebuild(policy, slow_report, slow_changes);

        REQUIRE(same_surface(fast.composed(), slow.composed()));
    }

    CHECK(ever_fast);
}

TEST("vdoc - muting a layer recomposes and says why")
{
    auto const registry = test_registry();
    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto changes = change_summary();

    auto base = direct_layer("base");
    stamp_transform(base, "e1");
    base.set(path_of("e1", "Transform", "x"), value::of(1.0));

    auto graph = op_graph();
    auto const head = graph.add(op_builder().set_raw(path_of("e1", "Transform", "x"), value::of(9.0)).build(graph));

    auto stack = layer_stack();
    (void)stack.push_direct_layer("base", base);
    auto const user = stack.push_graph_layer("user", graph, head);
    stack.rebuild(policy, report, changes);

    REQUIRE(stack.composed().get<vdoc_test::transform>(entity_id::of("e1"))->x == 9.0);

    stack.set_muted(user, true);

    auto stats = vdoc::layered_apply_stats();
    stack.apply(policy, report, changes, {}, &stats);

    // Not a fast path, and it names the reason: what muting changed is a whole layer's paths, which for a base layer
    // would be the whole document.
    CHECK(!stats.took_fast_path);
    CHECK(stats.fallback_reason == vdoc::apply_fallback_reason::structure_changed);

    CHECK(stack.composed().get<vdoc_test::transform>(entity_id::of("e1"))->x == 1.0);
    CHECK(stack.is_muted(user));
}

TEST("vdoc - layers that disagree about a component's schema version drop it")
{
    // The hazard is the DEFAULT rather than an edge case: op_builder::set always stamps, so every layer written through
    // the typed API carries a version, and the topmost stamp would otherwise describe only its own properties.
    auto const registry = test_registry();
    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto changes = change_summary();

    auto base = direct_layer("base");
    stamp_transform(base, "e1"); // version 2, with version-2 shaped properties
    base.set(path_of("e1", "Transform", "x"), value::of(1.0));
    base.set(path_of("e1", "Transform", "y"), value::of(2.0));

    // an override layer that also supplies data AND claims a different version
    auto graph = op_graph();
    auto const head = graph.add(op_builder()
                                    .set_raw(path_of("e1", "Transform", "$schema_version"), value::of(i64(1)))
                                    .set_raw(path_of("e1", "Transform", "x"), value::of(99.0))
                                    .build(graph));

    auto stack = layer_stack();
    (void)stack.push_direct_layer("base", base);
    (void)stack.push_graph_layer("user", graph, head);
    stack.rebuild(policy, report, changes);

    // Dropped and reported, rather than read at version 1 and parsed from version-2 data.
    CHECK(report.count_of(vdoc::diagnostic_kind::layered_schema_version_conflict) == 1);
    CHECK(stack.composed().get<vdoc_test::transform>(entity_id::of("e1")) == nullptr);

    // An override that does NOT stamp has no opinion, which is the shape an override is supposed to have.
    auto quiet = op_graph();
    auto const quiet_head
        = quiet.add(op_builder().set_raw(path_of("e1", "Transform", "x"), value::of(99.0)).build(quiet));

    auto ok = layer_stack();
    (void)ok.push_direct_layer("base", base);
    (void)ok.push_graph_layer("user", quiet, quiet_head);

    auto ok_report = parse_report();
    ok.rebuild(policy, ok_report, changes);

    CHECK(ok_report.count_of(vdoc::diagnostic_kind::layered_schema_version_conflict) == 0);
    REQUIRE(ok.composed().get<vdoc_test::transform>(entity_id::of("e1")) != nullptr);
    CHECK(ok.composed().get<vdoc_test::transform>(entity_id::of("e1"))->x == 99.0);
}

TEST("vdoc - a multi-value inside a layer survives composition")
{
    // Layering is conflict-free ACROSS layers, and says nothing about within one: the winning layer's whole writer list
    // replaces the lower one, so a contested path still reaches the policy exactly as it would unlayered.
    auto const registry = test_registry();
    auto report = parse_report();
    auto changes = change_summary();

    auto base = direct_layer("base");
    stamp_transform(base, "e1");
    base.set(path_of("e1", "Transform", "x"), value::of(1.0));

    auto graph = op_graph();
    auto const root = graph.add(op_builder().build(graph));
    op_id const from_root[] = {root};
    auto const left = graph.add(
        op_builder().set_parents(from_root).set_raw(path_of("e1", "Transform", "x"), value::of(10.0)).build(graph));
    auto const right = graph.add(
        op_builder().set_parents(from_root).set_raw(path_of("e1", "Transform", "x"), value::of(20.0)).build(graph));

    // the layer's head is a merge of the two, which leaves the path genuinely multi-valued
    op_id const both[] = {left, right};
    auto const merged = graph.add(op_builder().set_parents(both).build(graph));

    auto const policy = vdoc::default_parse_policy::create_with_local_head(registry, graph, merged);

    auto stack = layer_stack();
    (void)stack.push_direct_layer("base", base);
    (void)stack.push_graph_layer("user", graph, merged);
    stack.rebuild(policy, report, changes);

    // the conflict was resolved by the policy rather than silently taking the base
    CHECK(report.count_of(vdoc::diagnostic_kind::multi_valued_conflict) == 1);

    auto const* const t = stack.composed().get<vdoc_test::transform>(entity_id::of("e1"));
    REQUIRE(t != nullptr);
    CHECK((t->x == 10.0 || t->x == 20.0));
    CHECK(t->x != 1.0); // and the base did not win a path the layer above holds
}
