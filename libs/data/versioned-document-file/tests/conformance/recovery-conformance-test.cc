#include "store_fixture.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/value_debug.hh>

/// Recovery from an untrusted peer, on both arms.
///
/// The property under test is that a replica takes history from anyone and needs to trust none of it: the ids it
/// already holds commit to the bytes, so recomputing the hashes is the whole check.
/// What is added over the in-memory suite is the storage leg — a filled-in payload has to survive a reopen, and the
/// boundary a required snapshot draws has to survive one too.

using namespace cc::primitive_defines;
using namespace vdoc::file;
using namespace vdoc::file::test;

namespace
{
store_handle open_or_fail(store_medium& medium)
{
    auto opened = medium.open();
    REQUIRE(opened.has_value());
    return cc::move(opened.value());
}

vdoc::op_id publish_history(store& s, sample_history const& history)
{
    copy_ops_into(s, history.graph, history.ops);
    auto published = wait_for(s.publish({.refs = {{cc::string("main"), history.head()}}}));
    REQUIRE(published.has_value());
    return history.head();
}

/// A document published, then pruned at `history.ops[at]`, closed, and reopened.
/// What comes back is a replica missing everything behind the prune point — the state recovery exists for.
store_handle pruned_replica(store_medium& medium, sample_history const& history, isize at)
{
    auto const s = open_or_fail(medium);
    publish_history(*s, history);

    auto pruned = wait_for(*s, s->prune(history.ops[at]));
    REQUIRE(pruned.has_value());
    s->close();

    return open_or_fail(medium);
}

bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

/// Every op reachable from `head` recomputes to the id it is stored under.
bool all_verified(vdoc::op_graph const& graph, cc::span<vdoc::op_id const> ids)
{
    for (auto const& id : ids)
    {
        auto const* const op = graph.find(id);
        if (op == nullptr || vdoc::verify_op(*op) != vdoc::op_verification::verified)
            return false;
    }
    return true;
}
} // namespace

INVOCABLE_TEST("vdoc::file - a pruned replica recovers its history from a peer", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(10);
    auto const expected = materialize_to_text(history.graph, history.head());

    {
        auto const replica = pruned_replica(*medium, history, 6);

        // The peer is the untouched graph; the replica is what a prune left behind.
        auto const missing = skeleton_ids(replica->ops(), history.ops);
        REQUIRE(missing.size() == 6);
        CHECK(medium->first_snapshot_is_required());

        auto const batch = copy_received(history.graph, missing);
        auto const recovered = wait_for(*replica, replica->recover(views_of(batch)));

        REQUIRE(recovered.has_value());
        CHECK(recovered.value().skeletons_filled == 6);
        CHECK(recovered.value().ops_added == 0);

        // Nothing was taken on the sender's word: every op recomputes to the id the replica already expected.
        CHECK(all_verified(replica->ops(), history.ops));
        CHECK(skeleton_ids(replica->ops(), history.ops).empty());
        replica->close();
    }

    // The leg that proves the fill reached the file rather than only the resident graph.
    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().is_empty());
    CHECK(skeleton_ids(reopened->ops(), history.ops).empty());
    CHECK(all_verified(reopened->ops(), history.ops));
    CHECK(materialize_to_text(reopened->ops(), history.head()) == expected);
}

INVOCABLE_TEST("vdoc::file - a tampered op from a peer is rejected and the replica is intact", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(10);

    auto const replica = pruned_replica(*medium, history, 6);
    auto const missing = skeleton_ids(replica->ops(), history.ops);
    REQUIRE(missing.size() == 6);

    auto batch = copy_received(history.graph, missing);
    REQUIRE(batch.size() == 6);

    // Valid content, sent under an id that commits to different content — the honest shape of a tamper.
    batch[3].assignments = batch[4].assignments;

    auto const before = medium->snapshot_bytes();
    auto const recovered = wait_for(*replica, replica->recover(views_of(batch)));

    REQUIRE(recovered.has_error());

    // The batch is a set, so the five good ops ahead of the tampered one are not kept either.
    CHECK(skeleton_ids(replica->ops(), history.ops).size() == 6);
    CHECK(medium->first_snapshot_is_required());
    CHECK(same_bytes(before, medium->snapshot_bytes()));
}

INVOCABLE_TEST("vdoc::file - a required snapshot is demoted once its ancestry is filled in", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(10);
    auto const expected = materialize_to_text(history.graph, history.head());
    auto const boundary = history.ops[6];

    {
        auto const replica = pruned_replica(*medium, history, 6);
        REQUIRE(replica->snapshots().get_ptr(boundary) != nullptr);
        CHECK(replica->snapshots().get_ptr(boundary)->required);
        CHECK(replica->snapshot_cache().is_pinned(boundary));

        auto const batch = copy_received(history.graph, skeleton_ids(replica->ops(), history.ops));
        auto const recovered = wait_for(*replica, replica->recover(views_of(batch)));

        REQUIRE(recovered.has_value());
        CHECK(recovered.value().snapshots_demoted == 1);
        CHECK(!replica->snapshots().get_ptr(boundary)->required);

        // No longer load-bearing, so it is an ordinary cache entry again — and dropping it changes nothing but speed.
        CHECK(!replica->snapshot_cache().is_pinned(boundary));
        replica->snapshot_cache().clear_unpinned();
        CHECK(materialize_to_text(replica->ops(), history.head()) == expected);
        replica->close();
    }

    CHECK(!medium->first_snapshot_is_required());

    auto const reopened = open_or_fail(*medium);
    REQUIRE(reopened->snapshots().get_ptr(boundary) != nullptr);
    CHECK(!reopened->snapshots().get_ptr(boundary)->required);
    CHECK(!reopened->snapshot_cache().is_pinned(boundary));
}

INVOCABLE_TEST("vdoc::file - a partial recovery leaves the snapshot required and pinned", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(10);
    auto const boundary = history.ops[6];

    auto const replica = pruned_replica(*medium, history, 6);
    auto const missing = skeleton_ids(replica->ops(), history.ops);
    REQUIRE(missing.size() == 6);

    // Half the hole filled is still a hole, so what the snapshot stands in for is not back yet.
    auto const batch
        = copy_received(history.graph, cc::span<vdoc::op_id const>(missing).subspan({.offset = 3, .size = 3}));
    auto const recovered = wait_for(*replica, replica->recover(views_of(batch)));

    REQUIRE(recovered.has_value());
    CHECK(recovered.value().skeletons_filled == 3);
    CHECK(recovered.value().snapshots_demoted == 0);
    CHECK(replica->snapshots().get_ptr(boundary)->required);
    CHECK(replica->snapshot_cache().is_pinned(boundary));
}

INVOCABLE_TEST("vdoc::file - a batch forking below a still-required snapshot is refused", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto history = make_linear_history(10);

    // A branch off an op the prune will empty: its writers were superseded by ops that are about to become skeletons.
    auto const fork = add_branch(history.graph, history.ops[3], "peer");

    auto const replica = pruned_replica(*medium, history, 6);
    auto const missing = skeleton_ids(replica->ops(), history.ops);
    REQUIRE(missing.size() == 6);

    auto const before = medium->snapshot_bytes();
    {
        vdoc::op_id const just_fork[] = {fork};
        auto const batch = copy_received(history.graph, just_fork);
        auto const refused = wait_for(*replica, replica->recover(views_of(batch)));

        REQUIRE(refused.has_error());
        CHECK(!replica->ops().contains(fork));
        CHECK(same_bytes(before, medium->snapshot_bytes()));
    }

    // The same fork, sent WITH the ancestry that retires the snapshot, is accepted — which is what makes the rule a
    // boundary rather than a ban.
    auto together = cc::vector<vdoc::op_id>::create_copy_of(missing);
    together.push_back(fork);

    auto const batch = copy_received(history.graph, together);
    auto const recovered = wait_for(*replica, replica->recover(views_of(batch)));

    REQUIRE(recovered.has_value());
    CHECK(recovered.value().ops_added == 1);
    CHECK(recovered.value().skeletons_filled == 6);
    CHECK(recovered.value().snapshots_demoted == 1);
    CHECK(replica->ops().contains(fork));

    // Both heads materialize together, over history that is genuinely all there.
    vdoc::op_id const heads[] = {history.head(), fork};
    CHECK(replica->ops().materialize(heads).property_count() > 0);
}

INVOCABLE_TEST("vdoc::file - recovering the same history twice writes nothing the second time", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(10);

    auto const replica = pruned_replica(*medium, history, 6);
    auto const batch = copy_received(history.graph, skeleton_ids(replica->ops(), history.ops));

    auto const first = wait_for(*replica, replica->recover(views_of(batch)));
    REQUIRE(first.has_value());
    CHECK(first.value().skeletons_filled == 6);

    auto const settled = medium->snapshot_bytes();
    auto const second = wait_for(*replica, replica->recover(views_of(batch)));

    REQUIRE(second.has_value());
    CHECK(second.value() == recovery_result{});
    CHECK(same_bytes(settled, medium->snapshot_bytes()));
}

INVOCABLE_TEST("vdoc::file - recovering into a closed store is refused", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(10);

    auto const replica = pruned_replica(*medium, history, 6);
    auto const batch = copy_received(history.graph, skeleton_ids(replica->ops(), history.ops));
    replica->close();

    auto const refused = wait_for(*replica, replica->recover(views_of(batch)));
    CHECK(refused.has_error());
}
