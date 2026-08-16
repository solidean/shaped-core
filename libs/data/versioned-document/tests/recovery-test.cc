#include "op_graph_corpus.hh"

#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/recovery.hh>

using namespace cc::primitive_defines;

using vdoc::op_decode_error;
using vdoc::op_graph;
using vdoc::op_id;
using vdoc::received_op;

using namespace vdoc_test;

namespace
{
/// One op as it would sit in a receive buffer, owning its bytes so a test can tamper with them.
struct wire_op
{
    op_id id;
    cc::vector<op_id> parents;
    cc::vector<byte> metadata;
    cc::vector<byte> assignments;

    [[nodiscard]] received_op view() const
    {
        return received_op{.id = id, .parents = parents, .metadata_bytes = metadata, .assignment_bytes = assignments};
    }
};

/// What a peer would put on the wire for one op it holds.
[[nodiscard]] wire_op send(op_graph const& graph, op_id const& id)
{
    auto const* const o = graph.find(id);
    CC_ASSERT(o != nullptr && !o->is_skeleton(), "the sender does not have this op in full");

    auto const& p = o->payload.value();
    return wire_op{.id = o->id,
                   .parents = cc::vector<op_id>::create_copy_of(cc::span<op_id const>(o->parents)),
                   .metadata = cc::vector<byte>::create_copy_of(cc::span<byte const>(p.metadata_bytes)),
                   .assignments = cc::vector<byte>::create_copy_of(cc::span<byte const>(p.assignment_bytes))};
}

[[nodiscard]] cc::vector<wire_op> send_all(op_graph const& graph, cc::span<op_id const> ids)
{
    auto out = cc::vector<wire_op>();
    for (auto const& id : ids)
        out.push_back(send(graph, id));

    return out;
}

[[nodiscard]] cc::vector<received_op> views_of(cc::span<wire_op const> batch)
{
    auto out = cc::vector<received_op>();
    for (auto const& w : batch)
        out.push_back(w.view());

    return out;
}
} // namespace

TEST("vdoc - a received batch reconstructs the graph it came from")
{
    for (auto const& c : generate_corpus())
    {
        auto const batch = send_all(c.graph, c.ops);

        auto replica = op_graph();
        auto const result = vdoc::integrate(replica, views_of(batch));

        REQUIRE(result.has_value());
        CHECK(result.value().ops_added == c.ops.size());

        // The same ops mean the same materialization, and that is the only claim worth making about a replica.
        for (auto const& heads : c.head_sets)
            CHECK(same_document(replica.materialize(heads), c.graph.materialize(heads)));
    }
}

TEST("vdoc - integration is order-independent")
{
    for (auto const& c : generate_corpus())
    {
        auto reversed_ids = cc::vector<op_id>();
        for (isize i = c.ops.size() - 1; i >= 0; --i)
            reversed_ids.push_back(c.ops[i]);

        auto forward = op_graph();
        auto backward = op_graph();
        REQUIRE(vdoc::integrate(forward, views_of(send_all(c.graph, c.ops))).has_value());
        REQUIRE(vdoc::integrate(backward, views_of(send_all(c.graph, reversed_ids))).has_value());

        // An op whose parents have not arrived yet is a normal state, so the reverse order must land the same graph.
        for (auto const& heads : c.head_sets)
            CHECK(same_document(forward.materialize(heads), backward.materialize(heads)));
    }
}

TEST("vdoc - a tampered op in a received batch is rejected by id")
{
    auto sender = op_graph();
    auto const p = path_of("e1", "T", "x");

    write const one[] = {{.path = p, .value = 1}};
    write const two[] = {{.path = p, .value = 2}};
    auto const a = add_op(sender, {}, one);
    auto const b = add_op(sender, {}, two);

    // The honest shape of a tamper: valid content, sent under an id that commits to different content.
    auto tampered = send(sender, a);
    tampered.assignments = send(sender, b).assignments;

    auto replica = op_graph();
    received_op const batch[] = {tampered.view()};
    auto const result = vdoc::integrate(replica, batch);

    REQUIRE(result.has_error());
    CHECK(result.error().op == a);
    CHECK(result.error().reason == vdoc::integration_error::malformed);
    CHECK(result.error().decode_error == op_decode_error::hash_mismatch);
    CHECK(replica.size() == 0);
}

TEST("vdoc - parents that disagree with a held skeleton are refused")
{
    auto sender = op_graph();
    auto const p = path_of("e1", "T", "x");

    write const one[] = {{.path = p, .value = 1}};
    auto const root = add_op(sender, {}, one);

    op_id const from_root[] = {root};
    write const two[] = {{.path = p, .value = 2}};
    auto const head = add_op(sender, from_root, two);

    // A skeleton carries parents no hash has ever covered, so storage can hand back a wrong list and nothing upstream
    // would know.
    // Integration is the one moment that becomes detectable.
    auto replica = op_graph();
    auto corrupt = vdoc::try_decode_skeleton_op(head, {});
    REQUIRE(corrupt.has_value());
    (void)replica.add(cc::move(corrupt.value()));

    op_id const just_head[] = {head};
    auto const result = vdoc::integrate(replica, views_of(send_all(sender, just_head)));

    REQUIRE(result.has_error());
    CHECK(result.error().op == head);
    CHECK(result.error().reason == vdoc::integration_error::parents_disagree);
    CHECK(replica.find(head)->is_skeleton());
}

TEST("vdoc - a rejected batch leaves the replica exactly as it was")
{
    auto sender = op_graph();
    auto const p = path_of("e1", "T", "x");

    write const one[] = {{.path = p, .value = 1}};
    auto const root = add_op(sender, {}, one);

    op_id const from_root[] = {root};
    write const two[] = {{.path = p, .value = 2}};
    auto const child = add_op(sender, from_root, two);

    auto tampered = send(sender, child);
    tampered.assignments = send(sender, root).assignments;

    // The two good ops come FIRST, so a receiver that applied as it went would be left holding them.
    auto batch = cc::vector<wire_op>();
    batch.push_back(send(sender, root));
    batch.push_back(send(sender, child));
    batch.push_back(cc::move(tampered));

    auto replica = op_graph();
    auto const result = vdoc::integrate(replica, views_of(batch));

    REQUIRE(result.has_error());
    CHECK(replica.size() == 0);
}

TEST("vdoc - integrating fills a skeleton where add would not")
{
    auto sender = op_graph();
    auto const p = path_of("e1", "T", "x");

    write const one[] = {{.path = p, .value = 1}};
    auto const root = add_op(sender, {}, one);

    op_id const from_root[] = {root};
    write const two[] = {{.path = p, .value = 2}};
    auto const head = add_op(sender, from_root, two);

    op_id const heads[] = {head};
    auto replica = op_graph();
    REQUIRE(vdoc::integrate(replica, views_of(send_all(sender, sender.collect_reachable(heads)))).has_value());

    // Prune the replica back, which is what a pruned document holds where that op was.
    REQUIRE(replica.skeletonize(root));
    REQUIRE(replica.find(root)->is_skeleton());

    // add is idempotent by id, so it leaves a skeleton a skeleton — the blocker recovery exists to get past.
    (void)replica.add(*sender.find(root));
    CHECK(replica.find(root)->is_skeleton());

    op_id const just_root[] = {root};
    auto const result = vdoc::integrate(replica, views_of(send_all(sender, just_root)));

    REQUIRE(result.has_value());
    CHECK(result.value().skeletons_filled == 1);
    CHECK(result.value().ops_added == 0);
    CHECK(!replica.find(root)->is_skeleton());
    CHECK(vdoc::verify_op(*replica.find(root)) == vdoc::op_verification::verified);
    CHECK(same_document(replica.materialize(heads), sender.materialize(heads)));
}

TEST("vdoc - an op the replica already holds in full is verified and left alone")
{
    auto sender = op_graph();
    write const one[] = {{.path = path_of("e1", "T", "x"), .value = 1}};
    auto const root = add_op(sender, {}, one);

    op_id const just_root[] = {root};
    auto const batch = send_all(sender, just_root);

    auto replica = op_graph();
    REQUIRE(vdoc::integrate(replica, views_of(batch)).has_value());

    auto const again = vdoc::integrate(replica, views_of(batch));
    REQUIRE(again.has_value());
    CHECK(again.value().already_present == 1);
    CHECK(again.value().ops_added == 0);
    CHECK(replica.size() == 1);
}

TEST("vdoc - a peer cannot offer a skeleton, and it never reads as tampering")
{
    auto sender = op_graph();
    write const one[] = {{.path = path_of("e1", "T", "x"), .value = 1}};
    auto const root = add_op(sender, {}, one);

    auto empty = send(sender, root);
    empty.metadata.clear();
    empty.assignments.clear();

    auto replica = op_graph();
    received_op const batch[] = {empty.view()};
    auto const result = vdoc::integrate(replica, batch);

    REQUIRE(result.has_error());
    CHECK(replica.size() == 0);

    // Nothing to hash is malformed input, never a mismatch: a mismatch means corruption or tampering, and a peer with
    // nothing to send is neither.
    CHECK(result.error().decode_error != op_decode_error::hash_mismatch);
}
