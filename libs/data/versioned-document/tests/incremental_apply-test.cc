#include "components.hh"
#include "op_graph_corpus.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/incremental_parse.hh>
#include <versioned-document/op_builder.hh>

/// The incremental apply, measured against the full re-parse it exists to avoid.
///
/// Every test is the same property: **the fast path and the slow path produce the same document and the same report.**
/// `incremental_apply_options::force_full_reparse` exists so both can be run over identical input, and
/// `incremental_apply_stats::took_fast_path` is asserted so a green run cannot mean "it always fell back".
///
/// The reports agree **except for `unsupported_component_type`**, which is document-scoped and which an incremental
/// apply deliberately never retracts — see ../docs/concepts/interpretation.md.

using namespace cc::primitive_defines;

using vdoc::change_kind;
using vdoc::change_summary;
using vdoc::component_registry;
using vdoc::document;
using vdoc::entity_id;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::parse_report;

using namespace vdoc_test;

namespace inc_test
{
struct corpus_point;
}

/// The corpus writes i64s to `e`/`T`/`p0..pN`, so this is the component type that makes those into a real document.
/// Without it every corpus component would be unsupported and the comparison would be between two empty documents.
struct inc_test::corpus_point
{
    i64 p0 = 0;
    i64 p1 = 0;
    i64 p2 = 0;

    [[nodiscard]] friend bool operator==(corpus_point const&, corpus_point const&) = default;
};

template <>
struct vdoc::component_traits<inc_test::corpus_point>
{
    static constexpr cc::string_view type_name = "T";
    static constexpr i32 schema_version = 1;

    static void write(inc_test::corpus_point const& c, vdoc::component_writer& w)
    {
        w.set("p0", vdoc::value::of(c.p0));
        w.set("p1", vdoc::value::of(c.p1));
        w.set("p2", vdoc::value::of(c.p2));
    }

    static cc::optional<inc_test::corpus_point> parse(vdoc::property_reader const& r)
    {
        auto out = inc_test::corpus_point();
        if (auto const v = r.try_get("p0"); v.has_value())
            out.p0 = v.value().as_i64();
        if (auto const v = r.try_get("p1"); v.has_value())
            out.p1 = v.value().as_i64();
        if (auto const v = r.try_get("p2"); v.has_value())
            out.p2 = v.value().as_i64();
        return out;
    }
};

namespace
{
[[nodiscard]] component_registry test_registry()
{
    auto out = component_registry();
    out.register_component<vdoc_test::transform>();
    out.register_component<vdoc_test::mesh>();
    out.register_component<vdoc_test::tag>();
    out.register_component<inc_test::corpus_point>();
    return out;
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

        auto const* const pa = a.get<inc_test::corpus_point>(e);
        auto const* const pb = b.get<inc_test::corpus_point>(e);
        if ((pa == nullptr) != (pb == nullptr) || (pa != nullptr && !(*pa == *pb)))
            return false;
    }

    return true;
}

/// A report's entries, minus the document-scoped ones the incremental path never retracts.
[[nodiscard]] cc::vector<vdoc::diagnostic> entity_diagnostics(parse_report const& report)
{
    auto out = cc::vector<vdoc::diagnostic>();
    for (auto const& d : report.diagnostics)
        if (d.kind != vdoc::diagnostic_kind::unsupported_component_type)
            out.push_back(d);

    cc::sort(out,
             [](vdoc::diagnostic const& a, vdoc::diagnostic const& b)
             {
                 if (auto const by_path = a.path.compare_bytes(b.path); by_path != 0)
                     return by_path < 0;
                 return u8(a.kind) < u8(b.kind);
             });
    return out;
}

[[nodiscard]] bool same_diagnostics(parse_report const& a, parse_report const& b)
{
    auto const da = entity_diagnostics(a);
    auto const db = entity_diagnostics(b);
    if (da.size() != db.size())
        return false;

    for (isize i = 0; i < da.size(); ++i)
        if (!(da[i] == db[i]))
            return false;

    return true;
}

/// The document at `at`, parsed from scratch, plus the report that goes with it.
[[nodiscard]] document parse_at(op_graph const& graph, op_id const& at, vdoc::parse_policy const& policy, parse_report& report)
{
    report.clear();
    return vdoc::parse(graph.materialize(at), policy, report);
}

/// Every change the summary names, as a flat sorted list a test can compare.
[[nodiscard]] cc::vector<cc::string> summary_lines(change_summary const& s)
{
    auto const kind_name = [](change_kind k)
    {
        switch (k)
        {
        case change_kind::added:
            return "added";
        case change_kind::removed:
            return "removed";
        default:
            return "modified";
        }
    };

    auto out = cc::vector<cc::string>();
    for (auto const& e : s.entities)
        out.push_back(cc::format("entity {} {}", e.entity.as_string_view(), kind_name(e.kind)));
    for (auto const& c : s.components)
        out.push_back(cc::format("component {} {} {}", c.entity.as_string_view(), c.component.as_string_view(),
                                 kind_name(c.kind)));

    cc::sort(out);
    return out;
}
} // namespace

TEST("vdoc - a fast apply and a full re-parse agree, over the whole corpus")
{
    // The corpus is untyped, so a component type is put over its paths: every write is an i64, and the corpus's
    // component names are what the registry has to know.
    // That is enough to exercise the structure, which is what the corpus is for.
    auto const corpus = generate_corpus();
    REQUIRE(corpus.size() > 20);

    auto const registry = test_registry();
    auto ever_fast = false;
    auto ever_slow = false;

    for (auto const& c : corpus)
    {
        auto const policy = vdoc::default_parse_policy::create_with_registry(registry);

        for (auto const& to : c.ops)
        {
            auto const* const o = c.graph.find(to);
            if (o->parents.empty())
                continue;

            for (auto const& from : c.ops)
            {
                if (from == to)
                    continue;

                // Both runs start from the same place, and apply CONSUMES its document, so each gets its own.
                auto fast_report = parse_report();
                auto slow_report = parse_report();
                auto fast_changes = change_summary();
                auto slow_changes = change_summary();
                auto fast_stats = vdoc::incremental_apply_stats();
                auto slow_stats = vdoc::incremental_apply_stats();

                auto const fast = vdoc::apply(parse_at(c.graph, from, policy, fast_report), c.graph, from, to, policy,
                                              fast_report, fast_changes, {}, &fast_stats);

                auto const slow = vdoc::apply(parse_at(c.graph, from, policy, slow_report), c.graph, from, to, policy,
                                              slow_report, slow_changes, {.force_full_reparse = true}, &slow_stats);

                CHECK(same_surface(fast, slow));
                CHECK(same_diagnostics(fast_report, slow_report));
                CHECK(!slow_stats.took_fast_path);

                // and the slow path is the one that is right by construction, so it is worth naming
                auto oracle_report = parse_report();
                CHECK(same_surface(fast, parse_at(c.graph, to, policy, oracle_report)));

                ever_fast = ever_fast || fast_stats.took_fast_path;
                ever_slow = ever_slow || !fast_stats.took_fast_path;
            }
        }
    }

    // Neither vacuous: the corpus contains both single-parent chains and merges.
    CHECK(ever_fast);
    CHECK(ever_slow);
}

TEST("vdoc - the fast path fires on a chain and falls back on a merge")
{
    auto const registry = test_registry();
    auto graph = op_graph();

    auto const root = add_op(graph, {}, {});
    op_id const from_root[] = {root};

    property_write const wa[] = {{.path = path_of("e1", "Tag", "x"), .value = 1}};
    auto const a = add_op(graph, from_root, wa);
    property_write const wb[] = {{.path = path_of("e2", "Tag", "x"), .value = 2}};
    auto const b = add_op(graph, from_root, wb);

    op_id const both[] = {a, b};
    property_write const wm[] = {{.path = path_of("e3", "Tag", "x"), .value = 3}};
    auto const merge = add_op(graph, both, wm);

    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();

    {
        auto changes = change_summary();
        auto stats = vdoc::incremental_apply_stats();
        auto const doc
            = vdoc::apply(parse_at(graph, root, policy, report), graph, root, a, policy, report, changes, {}, &stats);
        CHECK(stats.took_fast_path);
        CHECK(stats.chain_ops == 1);
        CHECK(doc.contains(entity_id::of("e1")));
    }

    {
        auto changes = change_summary();
        auto stats = vdoc::incremental_apply_stats();
        auto const doc
            = vdoc::apply(parse_at(graph, a, policy, report), graph, a, merge, policy, report, changes, {}, &stats);
        CHECK(!stats.took_fast_path);
        CHECK(doc.contains(entity_id::of("e3")));
    }

    // and a chain longer than the bound falls back too
    {
        auto changes = change_summary();
        auto stats = vdoc::incremental_apply_stats();
        auto const doc = vdoc::apply(parse_at(graph, root, policy, report), graph, root, a, policy, report, changes,
                                     {.max_chain_ops = 0}, &stats);
        CHECK(!stats.took_fast_path);
        CHECK(doc.contains(entity_id::of("e1")));
    }
}

TEST("vdoc - applying nothing changes nothing")
{
    auto const registry = test_registry();
    auto graph = op_graph();

    property_write const w[] = {{.path = path_of("e1", "Tag", "x"), .value = 1}};
    auto const a = add_op(graph, {}, w);

    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto changes = change_summary();
    auto stats = vdoc::incremental_apply_stats();

    auto const doc = vdoc::apply(parse_at(graph, a, policy, report), graph, a, a, policy, report, changes, {}, &stats);

    CHECK(stats.took_fast_path);
    CHECK(stats.chain_ops == 0);
    CHECK(changes.is_empty());
    CHECK(doc.contains(entity_id::of("e1")));
}

TEST("vdoc - an entity killed and revived by $alive round-trips")
{
    // Deletion is interpretation rather than storage, so this is the case an apply gets wrong if it only copies
    // values instead of re-running the alive rules.
    auto const registry = test_registry();
    auto graph = op_graph();

    auto build = [&](cc::span<op_id const> parents, auto&& stage)
    {
        auto op = vdoc::op_builder();
        op.set_parents(parents);
        stage(op);
        return graph.add(op.build(graph));
    };

    auto const e = entity_id::of("wall");
    auto const created = build({}, [&](vdoc::op_builder& op) { op.set(e, vdoc_test::transform{.x = 1, .y = 2}); });

    op_id const from_created[] = {created};
    auto const killed = build(from_created, [&](vdoc::op_builder& op) { op.remove_entity(e); });

    op_id const from_killed[] = {killed};
    auto const revived = build(from_killed, [&](vdoc::op_builder& op) { op.restore_entity(e); });

    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);

    auto report = parse_report();
    auto changes = change_summary();
    auto stats = vdoc::incremental_apply_stats();

    auto doc = vdoc::apply(parse_at(graph, created, policy, report), graph, created, killed, policy, report, changes,
                           {}, &stats);
    CHECK(stats.took_fast_path);
    CHECK(!doc.contains(e));
    CHECK(doc.entity_count() == 0);

    auto const removed_lines = summary_lines(changes);
    REQUIRE(removed_lines.size() == 2);
    CHECK(removed_lines[0] == "component wall Transform removed");
    CHECK(removed_lines[1] == "entity wall removed");

    doc = vdoc::apply(cc::move(doc), graph, killed, revived, policy, report, changes, {}, &stats);
    CHECK(stats.took_fast_path);
    REQUIRE(doc.contains(e));
    REQUIRE(doc.get<vdoc_test::transform>(e) != nullptr);
    CHECK(doc.get<vdoc_test::transform>(e)->x == 1);

    auto slow_report = parse_report();
    CHECK(same_surface(doc, parse_at(graph, revived, policy, slow_report)));
    CHECK(same_diagnostics(report, slow_report));
}

TEST("vdoc - a component removed by $alive leaves the entity behind")
{
    auto const registry = test_registry();
    auto graph = op_graph();

    auto build = [&](cc::span<op_id const> parents, auto&& stage)
    {
        auto op = vdoc::op_builder();
        op.set_parents(parents);
        stage(op);
        return graph.add(op.build(graph));
    };

    auto const e = entity_id::of("wall");
    auto const created = build({},
                               [&](vdoc::op_builder& op)
                               {
                                   op.set(e, vdoc_test::transform{.x = 1});
                                   op.set(e, vdoc_test::mesh{.asset = "wall.obj"});
                               });

    op_id const from_created[] = {created};
    auto const stripped = build(from_created, [&](vdoc::op_builder& op)
                                { op.remove_component(e, vdoc::impl::component_type_of<vdoc_test::mesh>()); });

    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto changes = change_summary();
    auto stats = vdoc::incremental_apply_stats();

    auto const doc = vdoc::apply(parse_at(graph, created, policy, report), graph, created, stripped, policy, report,
                                 changes, {}, &stats);

    CHECK(stats.took_fast_path);
    CHECK(doc.contains(e));
    CHECK(doc.get<vdoc_test::mesh>(e) == nullptr);
    REQUIRE(doc.get<vdoc_test::transform>(e) != nullptr);

    auto const lines = summary_lines(changes);
    auto saw_removal = false;
    for (auto const& l : lines)
        saw_removal = saw_removal || l == "component wall Mesh removed";
    CHECK(saw_removal);

    auto slow_report = parse_report();
    CHECK(same_surface(doc, parse_at(graph, stripped, policy, slow_report)));
}

TEST("vdoc - a long chain of single-entity edits stays on the fast path and stays correct")
{
    // The workload the whole thing exists for: one entity moved, over and over.
    auto const registry = test_registry();
    auto graph = op_graph();

    auto seed = vdoc::op_builder();
    for (isize i = 0; i < 40; ++i)
        seed.set(entity_id::of(cc::format("e{}", i)), vdoc_test::transform{.x = f64(i)});
    auto head = graph.add(seed.build(graph));

    auto cache = vdoc::snapshot_cache();
    cache.install(head, vdoc::snapshot_document::create_owning_copy(graph.materialize(head)), /*pinned =*/true);

    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
    auto report = parse_report();
    auto doc = parse_at(graph, head, policy, report);

    for (isize step = 0; step < 60; ++step)
    {
        auto op = vdoc::op_builder();
        op.set_parents(cc::span<op_id const>(&head, 1));
        op.set(entity_id::of(cc::format("e{}", step % 40)), vdoc_test::transform{.x = f64(step) + 0.5});

        auto const previous = head;
        head = graph.add(op.build(graph, cache));
        REQUIRE(vdoc::advance_snapshot(graph, cache, previous, head));

        auto changes = change_summary();
        auto stats = vdoc::incremental_apply_stats();
        doc = vdoc::apply(cc::move(doc), graph, previous, head, policy, report, changes, {.cache = &cache}, &stats);

        REQUIRE(stats.took_fast_path);
        CHECK(stats.touched_entities == 1);
        CHECK(summary_lines(changes).size() == 2);
    }

    auto slow_report = parse_report();
    CHECK(same_surface(doc, parse_at(graph, head, policy, slow_report)));
    CHECK(same_diagnostics(report, slow_report));
    CHECK(doc.entity_count() == 40);
    CHECK(doc.get<vdoc_test::transform>(entity_id::of("e19"))->x == 59.5);
}

TEST("vdoc - a chain that makes a touched property multi-valued still agrees with a re-parse")
{
    // Multi-values INSIDE the touched set are fine: those entities go through the full selection-and-construction
    // path, exactly as a parse would run it.
    // What the single-parent gate rules out is an UNTOUCHED entity becoming multi-valued, which a chain cannot do.
    auto graph = op_graph();
    auto const registry = test_registry();

    // Component "T", not "Transform": transform's parse migrates from version 1 and never reads `x` at version 0,
    // so the conflicting property would never be resolved and the diagnostic this test is about would not be filed.
    auto const p = path_of("wall", "T", "p0");

    property_write const w0[] = {{.path = p, .value = 1}};
    auto const root = add_op(graph, {}, w0);

    op_id const from_root[] = {root};
    property_write const w1[] = {{.path = p, .value = 2}};
    auto const left = add_op(graph, from_root, w1);
    property_write const w2[] = {{.path = p, .value = 3}};
    auto const right = add_op(graph, from_root, w2);

    op_id const both[] = {left, right};
    property_write const w3[] = {{.path = path_of("other", "T", "p0"), .value = 9}};
    auto const merge = add_op(graph, both, w3);

    // now a single-parent child of the merge, so the chain from merge is a fast path over a multi-valued document
    op_id const from_merge[] = {merge};
    property_write const w4[] = {{.path = path_of("third", "T", "p0"), .value = 7}};
    auto const after = add_op(graph, from_merge, w4);

    CHECK(graph.materialize(merge).try_get(p)->is_multi_valued());

    auto const policy = vdoc::default_parse_policy::create_with_registry(registry);

    auto fast_report = parse_report();
    auto fast_changes = change_summary();
    auto fast_stats = vdoc::incremental_apply_stats();
    auto const fast = vdoc::apply(parse_at(graph, merge, policy, fast_report), graph, merge, after, policy, fast_report,
                                  fast_changes, {}, &fast_stats);

    auto slow_report = parse_report();
    auto slow_changes = change_summary();
    auto slow_stats = vdoc::incremental_apply_stats();
    auto const slow = vdoc::apply(parse_at(graph, merge, policy, slow_report), graph, merge, after, policy, slow_report,
                                  slow_changes, {.force_full_reparse = true}, &slow_stats);

    CHECK(fast_stats.took_fast_path);
    CHECK(!slow_stats.took_fast_path);
    CHECK(same_surface(fast, slow));
    CHECK(same_diagnostics(fast_report, slow_report));

    // the multi-value diagnostic survives the apply, because "wall" was not touched and its finding is still true
    CHECK(fast_report.count_of(vdoc::diagnostic_kind::multi_valued_conflict) == 1);
}
