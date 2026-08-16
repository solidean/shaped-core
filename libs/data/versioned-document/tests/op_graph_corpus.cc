#include "op_graph_corpus.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <versioned-document/value_builder.hh>

using vdoc::assignment;
using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::property_id;
using vdoc::property_path;
using vdoc::raw_document;
using vdoc::value;

namespace
{
using namespace cc::primitive_defines;

/// A fixed linear congruential generator, so a corpus case is reproducible from its seed forever.
/// The constants are the ones Numerical Recipes uses; nothing here needs statistical quality.
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

/// The ops with no children, which is what a caller would have refs on.
[[nodiscard]] cc::vector<op_id> tips_of(op_graph const& graph, cc::span<op_id const> ops)
{
    auto out = cc::vector<op_id>();
    for (auto const& id : ops)
        if (graph.children(id).empty())
            out.push_back(id);

    return out;
}
} // namespace

property_path vdoc_test::path_of(cc::string_view e, cc::string_view c, cc::string_view p)
{
    return property_path{.entity = entity_id::of(e),
                         .component = component_type_id::of(c),
                         .property = property_id::of(p)};
}

op_id vdoc_test::add_op(op_graph& graph, cc::span<op_id const> parents, cc::span<property_write const> writes)
{
    auto sorted_parents = cc::vector<op_id>::create_copy_of(parents);
    cc::sort(sorted_parents, op_id::by_bytes{});

    // the encoding rejects a duplicate parent, and a generated DAG can pick the same one twice
    auto unique_parents = cc::vector<op_id>();
    for (auto const& p : sorted_parents)
        if (unique_parents.empty() || !(unique_parents.back() == p))
            unique_parents.push_back(p);

    auto owned = cc::vector<value>();
    owned.reserve(writes.size());
    for (auto const& w : writes)
        owned.push_back(value::of_i64(w.value));

    auto entries = cc::vector<assignment>();
    for (isize i = 0; i < writes.size(); ++i)
        entries.push_back(assignment{.path = writes[i].path, .value = owned[i]});

    cc::sort(entries, [](assignment const& a, assignment const& b) { return a.path.compare_bytes(b.path) < 0; });

    auto const metadata = vdoc::value_builder::object().build();
    auto const metadata_bytes = cc::vector<byte>::create_copy_of(metadata.bytes());
    auto const assignment_bytes = vdoc::encode_assignments(entries);

    auto const id = vdoc::compute_op_id(unique_parents, metadata_bytes, assignment_bytes);
    auto decoded = vdoc::try_decode_op(id, unique_parents, metadata_bytes, assignment_bytes);
    CC_ASSERT(decoded.has_value(), "the harness built an op its own decoder rejects");

    return graph.add(cc::move(decoded.value()));
}

cc::vector<op_id> vdoc_test::writers_of(raw_document const& doc, property_path const& path)
{
    auto out = cc::vector<op_id>();
    auto const* const p = doc.try_get(path);
    if (p == nullptr)
        return out;

    for (auto const& w : p->writers)
        out.push_back(w.writer);

    return out;
}

bool vdoc_test::same_ids(cc::span<op_id const> a, cc::span<op_id const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}

bool vdoc_test::same_document(raw_document const& a, raw_document const& b)
{
    if (a.entities.size() != b.entities.size())
        return false;

    for (isize ei = 0; ei < a.entities.size(); ++ei)
    {
        auto const& ea = a.entities[ei];
        auto const& eb = b.entities[ei];
        if (!(ea.entity == eb.entity) || ea.value.components.size() != eb.value.components.size())
            return false;

        for (isize ci = 0; ci < ea.value.components.size(); ++ci)
        {
            auto const& ca = ea.value.components[ci];
            auto const& cb = eb.value.components[ci];
            if (!(ca.component == cb.component) || ca.value.properties.size() != cb.value.properties.size())
                return false;

            for (isize pi = 0; pi < ca.value.properties.size(); ++pi)
            {
                auto const& pa = ca.value.properties[pi];
                auto const& pb = cb.value.properties[pi];
                if (!(pa.property == pb.property) || pa.value.writers.size() != pb.value.writers.size())
                    return false;

                for (isize wi = 0; wi < pa.value.writers.size(); ++wi)
                {
                    auto const& wa = pa.value.writers[wi];
                    auto const& wb = pb.value.writers[wi];
                    if (!(wa.writer == wb.writer))
                        return false;

                    // byte equality, not value equality: the codec is canonical, so anything else is a difference
                    auto const ba = wa.value.bytes();
                    auto const bb = wb.value.bytes();
                    if (ba.size() != bb.size())
                        return false;
                    for (isize i = 0; i < ba.size(); ++i)
                        if (ba[i] != bb[i])
                            return false;
                }
            }
        }
    }

    return true;
}

cc::vector<op_id> vdoc_test::oracle_writers(op_graph const& graph, cc::span<op_id const> heads, property_path const& path)
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

    cc::sort(out, op_id::by_bytes{});
    return out;
}

namespace
{
/// Fills in the head sets every case shares: the last op, every tip, and the shapes that stress a snapshot.
void finish_case(vdoc_test::corpus_case& c)
{
    if (c.ops.empty())
        return;

    c.head_sets.push_back(cc::vector<op_id>{c.ops.back()});

    auto const tips = tips_of(c.graph, c.ops);
    if (tips.size() > 1)
        c.head_sets.push_back(cc::vector<op_id>::create_copy_of(tips));

    // {ancestor, descendant} — legal, and the exact shape a snapshot at BOTH would fabricate a multi-value on.
    if (c.ops.size() >= 3)
        c.head_sets.push_back(cc::vector<op_id>{c.ops.back(), c.ops[c.ops.size() / 2]});

    // two unrelated tips, where the case has them
    if (tips.size() >= 2)
        c.head_sets.push_back(cc::vector<op_id>{tips[0], tips[tips.size() - 1]});
}

[[nodiscard]] vdoc_test::corpus_case make_linear(isize length, isize path_count)
{
    auto c = vdoc_test::corpus_case{.name = cc::format("linear-{}x{}", length, path_count)};
    for (isize i = 0; i < path_count; ++i)
        c.paths.push_back(vdoc_test::path_of("e", "T", cc::format("p{}", i)));

    auto prev = cc::vector<op_id>();
    for (isize i = 0; i < length; ++i)
    {
        vdoc_test::property_write const w[] = {{.path = c.paths[i % path_count], .value = i}};
        auto const id = vdoc_test::add_op(c.graph, prev, w);
        c.ops.push_back(id);
        prev = cc::vector<op_id>{id};
    }

    finish_case(c);
    return c;
}

/// A merge over two branches that both write the same path — the canonical two-value case.
[[nodiscard]] vdoc_test::corpus_case make_diamond(bool merge_writes)
{
    auto c = vdoc_test::corpus_case{.name = merge_writes ? cc::string("diamond-merge-writes") : cc::string("diamond")};
    c.paths.push_back(vdoc_test::path_of("e", "T", "p"));

    vdoc_test::property_write const w0[] = {{.path = c.paths[0], .value = 0}};
    auto const root = vdoc_test::add_op(c.graph, {}, w0);
    c.ops.push_back(root);

    op_id const from_root[] = {root};
    vdoc_test::property_write const w1[] = {{.path = c.paths[0], .value = 1}};
    vdoc_test::property_write const w2[] = {{.path = c.paths[0], .value = 2}};
    auto const left = vdoc_test::add_op(c.graph, from_root, w1);
    auto const right = vdoc_test::add_op(c.graph, from_root, w2);
    c.ops.push_back(left);
    c.ops.push_back(right);

    op_id const both[] = {left, right};
    vdoc_test::property_write const w3[] = {{.path = c.paths[0], .value = 3}};
    c.ops.push_back(vdoc_test::add_op(
        c.graph, both,
        merge_writes ? cc::span<vdoc_test::property_write const>(w3) : cc::span<vdoc_test::property_write const>()));

    finish_case(c);
    return c;
}

/// Two merges over the same two branches, which is the shape a single merge base does not cover.
[[nodiscard]] vdoc_test::corpus_case make_criss_cross()
{
    auto c = vdoc_test::corpus_case{.name = "criss-cross"};
    c.paths.push_back(vdoc_test::path_of("e", "T", "p"));
    c.paths.push_back(vdoc_test::path_of("e", "T", "q"));

    auto const root = vdoc_test::add_op(c.graph, {}, {});
    c.ops.push_back(root);

    op_id const from_root[] = {root};
    vdoc_test::property_write const wa[] = {{.path = c.paths[0], .value = 1}};
    vdoc_test::property_write const wb[] = {{.path = c.paths[1], .value = 2}};
    auto const a = vdoc_test::add_op(c.graph, from_root, wa);
    auto const b = vdoc_test::add_op(c.graph, from_root, wb);
    c.ops.push_back(a);
    c.ops.push_back(b);

    op_id const ab[] = {a, b};
    vdoc_test::property_write const wm1[] = {{.path = c.paths[0], .value = 3}};
    vdoc_test::property_write const wm2[] = {{.path = c.paths[1], .value = 4}};
    c.ops.push_back(vdoc_test::add_op(c.graph, ab, wm1));
    c.ops.push_back(vdoc_test::add_op(c.graph, ab, wm2));

    finish_case(c);
    return c;
}

/// Four concurrent writers of one path, never merged — a genuinely multi-valued property.
[[nodiscard]] vdoc_test::corpus_case make_multi_valued()
{
    auto c = vdoc_test::corpus_case{.name = "multi-valued"};
    c.paths.push_back(vdoc_test::path_of("e", "T", "p"));

    auto const root = vdoc_test::add_op(c.graph, {}, {});
    c.ops.push_back(root);

    op_id const from_root[] = {root};
    for (isize i = 0; i < 4; ++i)
    {
        vdoc_test::property_write const w[] = {{.path = c.paths[0], .value = i}};
        c.ops.push_back(vdoc_test::add_op(c.graph, from_root, w));
    }

    finish_case(c);
    return c;
}

/// A generated DAG: mostly linear, with branches and merges at the requested rate.
///
/// This is the shape real editing produces — long linear runs with occasional local splits — so it is what the
/// snapshot cache is actually tuned against.
[[nodiscard]] vdoc_test::corpus_case make_generated(u32 seed, isize op_count, isize path_count, u32 merge_percent)
{
    auto c = vdoc_test::corpus_case{
        .name = cc::format("generated-s{}-n{}-p{}-m{}", seed, op_count, path_count, merge_percent)};
    for (isize i = 0; i < path_count; ++i)
        c.paths.push_back(vdoc_test::path_of("e", "T", cc::format("p{}", i)));

    auto rng = lcg{.state = seed * 2654435761u + 1u};

    for (isize i = 0; i < op_count; ++i)
    {
        auto parents = cc::vector<op_id>();
        if (!c.ops.empty())
        {
            auto const tips = tips_of(c.graph, c.ops);
            auto const& pool = tips.empty() ? c.ops : tips;
            parents.push_back(pool[rng.next_below(u32(pool.size()))]);

            // a merge takes a second parent from anywhere, which is what makes branches actually rejoin
            if (pool.size() > 1 && rng.next_below(100) < merge_percent)
            {
                auto const other = pool[rng.next_below(u32(pool.size()))];
                if (!(other == parents[0]))
                    parents.push_back(other);
            }
        }

        auto writes = cc::vector<vdoc_test::property_write>();
        auto const write_count = rng.next_below(u32(path_count) + 1);
        for (u32 w = 0; w < write_count; ++w)
            writes.push_back({.path = c.paths[rng.next_below(u32(path_count))], .value = i});

        // duplicate paths in one op are not encodable, so keep the first of each
        auto unique = cc::vector<vdoc_test::property_write>();
        for (auto const& w : writes)
        {
            auto seen = false;
            for (auto const& u : unique)
                seen = seen || u.path == w.path;
            if (!seen)
                unique.push_back(w);
        }

        c.ops.push_back(vdoc_test::add_op(c.graph, parents, unique));
    }

    finish_case(c);
    return c;
}
} // namespace

cc::vector<vdoc_test::corpus_case> vdoc_test::generate_corpus()
{
    auto out = cc::vector<corpus_case>();

    out.push_back(make_linear(16, 1));
    out.push_back(make_linear(16, 3));
    out.push_back(make_diamond(false));
    out.push_back(make_diamond(true));
    out.push_back(make_criss_cross());
    out.push_back(make_multi_valued());

    // Small enough that the O(n^2) install-at-every-pair sweep stays fast, varied enough to hit every merge shape.
    for (u32 seed = 0; seed < 6; ++seed)
        for (auto const path_count : {isize(1), isize(3)})
            for (auto const merge_percent : {u32(0), u32(20), u32(50)})
                out.push_back(make_generated(seed, 14, path_count, merge_percent));

    return out;
}
