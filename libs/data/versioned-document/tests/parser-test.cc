#include "components.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/parse.hh>
#include <versioned-document/value_builder.hh>

using namespace cc::primitive_defines;

using vdoc::component_registry;
using vdoc::component_type_id;
using vdoc::default_parse_policy;
using vdoc::diagnostic_kind;
using vdoc::document;
using vdoc::entity_id;
using vdoc::op_builder;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::parse_report;
using vdoc::property_id;
using vdoc::value;

using vdoc_test::mesh;
using vdoc_test::tag;
using vdoc_test::transform;

namespace
{
[[nodiscard]] component_registry full_registry()
{
    auto r = component_registry();
    r.register_component<transform>();
    r.register_component<mesh>();
    r.register_component<tag>();
    return r;
}

/// Adds one op extending `parents`, and returns the new head.
[[nodiscard]] op_id commit(op_graph& graph, cc::span<op_id const> parents, auto&& stage)
{
    auto builder = op_builder{};
    builder.set_parents(parents);
    stage(builder);
    return graph.add(builder.build(graph));
}

[[nodiscard]] op_id commit(op_graph& graph, op_id const& parent, auto&& stage)
{
    return commit(graph, cc::span<op_id const>(&parent, 1), stage);
}

[[nodiscard]] op_id commit(op_graph& graph, auto&& stage)
{
    return commit(graph, cc::span<op_id const>(), stage);
}

/// Every diagnostic of one kind, in report order.
[[nodiscard]] cc::vector<vdoc::diagnostic> diagnostics_of(parse_report const& report, diagnostic_kind kind)
{
    auto out = cc::vector<vdoc::diagnostic>();
    for (auto const& d : report.diagnostics)
        if (d.kind == kind)
            out.push_back(d);

    return out;
}

[[nodiscard]] vdoc::property_path path_of(entity_id e, cc::string_view component, cc::string_view property)
{
    return {.entity = e, .component = component_type_id::of(component), .property = property_id::of(property)};
}
} // namespace

TEST("vdoc - an unknown component type lands as one diagnostic while every known component still parses")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const f = entity_id::of("wall-18");

    auto head = commit(graph, [&](op_builder& b) { b.set(e, transform{.x = 1, .y = 2}); });
    head = commit(graph, head,
                  [&](op_builder& b)
                  {
                      b.set_raw(e, component_type_id::of("Unknown"), property_id::of("k"), value::of(1));
                      b.set_raw(f, component_type_id::of("Unknown"), property_id::of("k"), value::of(2));
                      b.set(f, transform{.x = 3, .y = 4});
                  });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    CHECK(doc.entity_count() == 2);
    REQUIRE(doc.get<transform>(e) != nullptr);
    CHECK(doc.get<transform>(e)->x == 1);
    CHECK(doc.get<transform>(f)->x == 3);

    // Twice in the raw document, once in the report.
    auto const unsupported = diagnostics_of(report, diagnostic_kind::unsupported_component_type);
    REQUIRE(unsupported.size() == 1);
    CHECK(unsupported[0].path.component == component_type_id::of("Unknown"));
    CHECK(unsupported[0].path.entity.empty());
}

TEST("vdoc - a registry subset parses a superset document and reports exactly the missing types")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const head = commit(graph,
                             [&](op_builder& b)
                             {
                                 b.set(e, transform{.x = 1});
                                 b.set(e, mesh{.asset = "brick"});
                                 b.set(e, tag{});
                             });

    auto subset = component_registry();
    subset.register_component<transform>();

    auto const policy = default_parse_policy::create_with_registry(subset);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    CHECK(doc.has<transform>(e));
    CHECK(doc.count_of(component_type_id::of("Mesh")) == 0);

    auto const unsupported = diagnostics_of(report, diagnostic_kind::unsupported_component_type);
    REQUIRE(unsupported.size() == 2);
    CHECK(unsupported[0].path.component == component_type_id::of("Mesh"));
    CHECK(unsupported[1].path.component == component_type_id::of("Tag"));
}

TEST("vdoc - $alive false drops the component and leaves its siblings")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto head = commit(graph,
                       [&](op_builder& b)
                       {
                           b.set(e, transform{.x = 1});
                           b.set(e, mesh{.asset = "brick"});
                       });
    head = commit(graph, head, [&](op_builder& b) { b.set_alive(e, component_type_id::of("Mesh"), false); });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    CHECK(doc.contains(e));
    CHECK(doc.has<transform>(e));
    CHECK(!doc.has<mesh>(e));

    // Deletion is normal, so an uncontested one says nothing.
    CHECK(report.diagnostics.empty());
}

TEST("vdoc - $alive false on $entity drops the whole entity")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const f = entity_id::of("wall-18");

    auto head = commit(graph,
                       [&](op_builder& b)
                       {
                           b.set(e, transform{.x = 1});
                           b.set(f, transform{.x = 2});
                       });
    head = commit(graph, head, [&](op_builder& b) { b.set_entity_alive(e, false); });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    CHECK(!doc.contains(e));
    CHECK(doc.contains(f));
    CHECK(doc.entity_count() == 1);
    CHECK(report.diagnostics.empty());
}

TEST("vdoc - a later write undeletes")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto head = commit(graph, [&](op_builder& b) { b.set(e, transform{.x = 1}); });
    head = commit(graph, head, [&](op_builder& b) { b.set_entity_alive(e, false); });
    head = commit(graph, head, [&](op_builder& b) { b.set_entity_alive(e, true); });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    CHECK(doc.contains(e));
    CHECK(doc.has<transform>(e));
}

TEST("vdoc - a contested $alive keeps the thing alive and files contested_alive")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const base = commit(graph, [&](op_builder& b) { b.set(e, transform{.x = 1}); });
    auto const dead = commit(graph, base, [&](op_builder& b) { b.set_entity_alive(e, false); });
    auto const alive = commit(graph, base, [&](op_builder& b) { b.set_entity_alive(e, true); });

    op_id const heads[] = {dead, alive};

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(heads), policy, report);

    // Resurrecting is recoverable and vanishing is not.
    CHECK(doc.contains(e));
    CHECK(doc.has<transform>(e));
    REQUIRE(report.count_of(diagnostic_kind::contested_alive) == 1);
    CHECK(diagnostics_of(report, diagnostic_kind::contested_alive)[0].path.entity == e);
}

TEST("vdoc - agreed multi-values collapse with no diagnostic and one agreed entry")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    // Two concurrent writers of the same bytes: structurally multi-valued, and not a problem.
    // The metadata differs only so the two ops are not the same op — add is keyed by content hash, so identical
    // assignments under identical parents would collapse to one entry and there would be nothing to agree about.
    auto const base = commit(graph, [&](op_builder& b) { b.set(e, transform{.x = 1, .y = 1}); });
    auto const a = commit(graph, base,
                          [&](op_builder& b)
                          {
                              b.set_metadata(value::of("author-a"));
                              b.set(e, transform{.x = 5, .y = 1});
                          });
    auto const c = commit(graph, base,
                          [&](op_builder& b)
                          {
                              b.set_metadata(value::of("author-b"));
                              b.set(e, transform{.x = 5, .y = 1});
                          });

    op_id const heads[] = {a, c};

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(heads), policy, report);

    REQUIRE(doc.get<transform>(e) != nullptr);
    CHECK(doc.get<transform>(e)->x == 5);
    CHECK(report.diagnostics.empty());

    auto found = false;
    for (auto const& m : report.agreed_multi_values)
        if (m.path.property == property_id::of("x"))
        {
            found = true;
            CHECK(m.writer_count == 2);
        }
    CHECK(found);
}

TEST("vdoc - disagreeing writers with no local head resolve to the smallest op id")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const base = commit(graph, [&](op_builder& b) { b.set(e, transform{.x = 1}); });
    auto const a = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 5}); });
    auto const c = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 9}); });

    op_id const heads[] = {a, c};
    auto const raw = graph.materialize(heads);

    auto const* const prop = raw.try_get(path_of(e, "Transform", "x"));
    REQUIRE(prop != nullptr);
    REQUIRE(prop->is_multi_valued());
    auto const expected = prop->writers[0].value.as_f64();

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(raw, policy, report);

    CHECK(doc.get<transform>(e)->x == expected);

    auto const conflicts = diagnostics_of(report, diagnostic_kind::multi_valued_conflict);
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].chosen_writer == prop->writers[0].writer);
    CHECK(conflicts[0].writer_count == 2);
    CHECK(report.count_of(diagnostic_kind::remote_conflict) == 0);
}

TEST("vdoc - exactly one writer inside the local closure wins, with a remote conflict")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const base = commit(graph, [&](op_builder& b) { b.set(e, transform{.x = 1}); });
    auto const mine = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 5}); });
    auto const theirs = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 9}); });

    op_id const heads[] = {mine, theirs};

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_local_head(registry, graph, mine);
    CHECK(policy.has_local_closure());

    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(heads), policy, report);

    CHECK(doc.get<transform>(e)->x == 5);

    auto const conflicts = diagnostics_of(report, diagnostic_kind::remote_conflict);
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].chosen_writer == mine);
    CHECK(report.count_of(diagnostic_kind::multi_valued_conflict) == 0);
}

TEST("vdoc - a local closure holding every writer falls back to the smallest op id")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const base = commit(graph, [&](op_builder& b) { b.set(e, transform{.x = 1}); });
    auto const a = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 5}); });
    auto const c = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 9}); });

    op_id const heads[] = {a, c};
    auto const raw = graph.materialize(heads);

    // A local closure that reaches both writers leaves nothing to prefer.
    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_local_heads(registry, graph, heads);

    auto report = parse_report();
    auto const doc = vdoc::parse(raw, policy, report);

    auto const* const prop = raw.try_get(path_of(e, "Transform", "x"));
    CHECK(doc.get<transform>(e)->x == prop->writers[0].value.as_f64());
    CHECK(report.count_of(diagnostic_kind::multi_valued_conflict) == 1);
    CHECK(report.count_of(diagnostic_kind::remote_conflict) == 0);
}

TEST("vdoc - an older schema version migrates, and the next write re-stamps at the current one")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const type = component_type_id::of("Transform");

    // A version-1 Transform, written by hand: one `pos` array and a stamp of 1.
    auto const head = commit(graph,
                             [&](op_builder& b)
                             {
                                 b.set_raw(e, type, vdoc::reserved::schema_version(), value::of(1));
                                 b.set_raw(e, type, property_id::of("pos"),
                                           vdoc::value_builder::array().push(3.0).push(4.0).build());
                             });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    REQUIRE(doc.get<transform>(e) != nullptr);
    CHECK(doc.get<transform>(e)->x == 3.0);
    CHECK(doc.get<transform>(e)->y == 4.0);
    CHECK(report.diagnostics.empty());

    // Writing it back stamps the current version, and the stored version-1 op is untouched.
    auto const head2 = commit(graph, head, [&](op_builder& b) { b.set(e, *doc.get<transform>(e)); });
    auto const raw2 = graph.materialize(head2);
    CHECK(raw2.try_get({.entity = e, .component = type, .property = vdoc::reserved::schema_version()})->single().as_i64()
          == 2);
    CHECK(graph.find(head)->id == head);
}

TEST("vdoc - a newer schema version skips the component and leaves the stored data alone")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const type = component_type_id::of("Transform");

    auto const head = commit(graph,
                             [&](op_builder& b)
                             {
                                 b.set_raw(e, type, vdoc::reserved::schema_version(), value::of(99));
                                 b.set_raw(e, type, property_id::of("x"), value::of(1.0));
                                 b.set(e, mesh{.asset = "brick"});
                             });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const raw = graph.materialize(head);
    auto const doc = vdoc::parse(raw, policy, report);

    CHECK(!doc.has<transform>(e));
    CHECK(doc.has<mesh>(e));

    auto const skipped = diagnostics_of(report, diagnostic_kind::unknown_schema_version);
    REQUIRE(skipped.size() == 1);
    CHECK(skipped[0].path.component == type);
    CHECK(skipped[0].path.entity == e);

    // Untouched, for a build that does understand it.
    CHECK(raw.try_get(path_of(e, "Transform", "x"))->single().as_f64() == 1.0);
}

TEST("vdoc - a contested schema version skips the component")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const type = component_type_id::of("Transform");

    auto const base = commit(graph, [&](op_builder& b) { b.set_raw(e, type, property_id::of("x"), value::of(1.0)); });
    auto const a = commit(graph, base,
                          [&](op_builder& b) { b.set_raw(e, type, vdoc::reserved::schema_version(), value::of(1)); });
    auto const c = commit(graph, base,
                          [&](op_builder& b) { b.set_raw(e, type, vdoc::reserved::schema_version(), value::of(2)); });

    op_id const heads[] = {a, c};

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(heads), policy, report);

    CHECK(!doc.has<transform>(e));
    CHECK(report.count_of(diagnostic_kind::unknown_schema_version) == 1);
}

TEST("vdoc - a component with no stamp at all parses at version 0")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const type = component_type_id::of("Transform");

    // Nothing stamped it, which is a set_raw document rather than an unknown version.
    auto const head = commit(
        graph, [&](op_builder& b)
        { b.set_raw(e, type, property_id::of("pos"), vdoc::value_builder::array().push(7.0).push(8.0).build()); });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    REQUIRE(doc.get<transform>(e) != nullptr);
    CHECK(doc.get<transform>(e)->x == 7.0);
    CHECK(report.diagnostics.empty());
}

TEST("vdoc - a component whose parse returns empty is dropped without a diagnostic")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    // A Mesh with no asset: the traits drop it, which is not a failure.
    auto const head
        = commit(graph,
                 [&](op_builder& b)
                 {
                     b.set_raw(e, component_type_id::of("Mesh"), vdoc::reserved::schema_version(), value::of(1));
                     b.set(e, transform{.x = 1});
                 });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    CHECK(!doc.has<mesh>(e));
    CHECK(doc.has<transform>(e));
    CHECK(doc.count_of(component_type_id::of("Mesh")) == 0);
    CHECK(report.diagnostics.empty());
}

TEST("vdoc - parsing never mutates the graph")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const base = commit(graph, [&](op_builder& b) { b.set(e, transform{.x = 1}); });
    auto const a = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 5}); });
    auto const c = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 9}); });

    op_id const heads[] = {a, c};

    auto const before_size = graph.size();
    auto const before = graph.collect_reachable(heads);

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(heads), policy, report);
    CHECK(doc.entity_count() == 1);

    auto const after = graph.collect_reachable(heads);
    CHECK(graph.size() == before_size);
    REQUIRE(after.size() == before.size());
    for (isize i = 0; i < after.size(); ++i)
        CHECK(after[i] == before[i]);
}

TEST("vdoc - parsing the same raw document twice gives identical documents and identical report order")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");
    auto const f = entity_id::of("wall-18");

    auto const base = commit(graph,
                             [&](op_builder& b)
                             {
                                 b.set(e, transform{.x = 1});
                                 b.set(e, mesh{.asset = "brick"});
                                 b.set(f, tag{});
                                 b.set_raw(f, component_type_id::of("Unknown"), property_id::of("k"), value::of(1));
                             });
    auto const a = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 5}); });
    auto const c = commit(graph, base, [&](op_builder& b) { b.set(e, transform{.x = 9}); });

    op_id const heads[] = {a, c};
    auto const raw = graph.materialize(heads);

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);

    auto report1 = parse_report();
    auto const doc1 = vdoc::parse(raw, policy, report1);
    auto report2 = parse_report();
    auto const doc2 = vdoc::parse(raw, policy, report2);

    REQUIRE(doc1.entity_count() == doc2.entity_count());
    for (isize i = 0; i < doc1.entity_count(); ++i)
        CHECK(doc1.entities()[i] == doc2.entities()[i]);

    auto const types1 = doc1.component_types();
    auto const types2 = doc2.component_types();
    REQUIRE(types1.size() == types2.size());
    for (isize i = 0; i < types1.size(); ++i)
        CHECK(types1[i] == types2[i]);

    CHECK(*doc1.get<transform>(e) == *doc2.get<transform>(e));
    CHECK(*doc1.get<mesh>(e) == *doc2.get<mesh>(e));

    REQUIRE(report1.diagnostics.size() == report2.diagnostics.size());
    for (isize i = 0; i < report1.diagnostics.size(); ++i)
        CHECK(report1.diagnostics[i] == report2.diagnostics[i]);

    REQUIRE(report1.agreed_multi_values.size() == report2.agreed_multi_values.size());
    for (isize i = 0; i < report1.agreed_multi_values.size(); ++i)
        CHECK(report1.agreed_multi_values[i] == report2.agreed_multi_values[i]);
}

TEST("vdoc - a document outlives the graph it was parsed from")
{
    auto const e = entity_id::of("wall-17");
    auto const registry = full_registry();

    auto doc = document();
    {
        auto graph = op_graph();
        auto const head
            = commit(graph, [&](op_builder& b) { b.set(e, mesh{.asset = "a-long-asset-name-past-any-inline-buffer"}); });

        auto const policy = default_parse_policy::create_with_registry(registry);
        auto report = parse_report();
        doc = vdoc::parse(graph.materialize(head), policy, report);
    }

    REQUIRE(doc.get<mesh>(e) != nullptr);
    CHECK(doc.get<mesh>(e)->asset == "a-long-asset-name-past-any-inline-buffer");
}

TEST("vdoc - an entity whose every component is unknown still exists")
{
    auto graph = op_graph();
    auto const e = entity_id::of("wall-17");

    auto const head = commit(graph, [&](op_builder& b)
                             { b.set_raw(e, component_type_id::of("Unknown"), property_id::of("k"), value::of(1)); });

    auto const registry = full_registry();
    auto const policy = default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto const doc = vdoc::parse(graph.materialize(head), policy, report);

    CHECK(doc.contains(e));
    CHECK(doc.entity_count() == 1);
    CHECK(doc.component_types().empty());
    CHECK(report.count_of(diagnostic_kind::unsupported_component_type) == 1);
}
