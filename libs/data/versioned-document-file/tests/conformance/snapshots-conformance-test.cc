#include "store_fixture.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/snapshot_cache.hh>
#include <versioned-document/value_debug.hh>

/// Snapshots and pruning, on both arms.
///
/// The property under test is the same one the in-memory suite proves: a document materializes to what it always did.
/// What is added here is that it survives the round trip through storage — and that a snapshot the file cannot decode
/// is an issue or a hard failure depending on nothing but its `required` flag.

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

/// Publishes a whole history under "main" and returns its head.
vdoc::op_id publish_history(store& s, sample_history const& history)
{
    copy_ops_into(s, history.graph, history.ops);
    auto published = wait_for(s.publish({.refs = {{cc::string("main"), history.head()}}}));
    REQUIRE(published.has_value());
    return history.head();
}

/// The document as text, materialized through the store's own cache.
cc::string materialize_through_store(store& s, vdoc::op_id const& head)
{
    auto const raw = s.ops().materialize(head, s.snapshot_cache());

    auto text = cc::string();
    for (auto const& entity : raw.entities)
        for (auto const& component : entity.value.components)
            for (auto const& property : component.value.properties)
                for (auto const& writer : property.value.writers)
                    text += cc::format("{}/{}/{} = {}\n", entity.entity.as_string_view(),
                                       component.component.as_string_view(), property.property.as_string_view(),
                                       vdoc::to_debug_string(writer.value));
    return text;
}
/// Byte equality over two medium dumps, since cc::vector carries no equality of its own.
bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

INVOCABLE_TEST("vdoc::file - a published snapshot round-trips and is used", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(8);
    auto const expected = materialize_to_text(history.graph, history.head());
    vdoc::op_id const head_span[] = {history.head()};

    {
        auto const s = open_or_fail(*medium);
        publish_history(*s, history);

        // Explicit, because nothing installs a snapshot behind the caller's back.
        REQUIRE(vdoc::install_snapshot(s->ops(), history.head(), s->snapshot_cache()));
        vdoc::op_id const ops[] = {history.head()};
        auto written = wait_for(s->publish_snapshots(ops));
        REQUIRE(written.has_value());
        CHECK(written.value().snapshots_written == 1);
        s->close();
    }

    CHECK(medium->count_snapshots() == 1);
    CHECK(!medium->first_snapshot_is_required());

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().is_empty());
    REQUIRE(reopened->snapshots().size() == 1);

    auto const* entry = reopened->snapshots().get_ptr(history.head());
    REQUIRE(entry != nullptr);
    CHECK(entry->decoded);
    CHECK(!entry->required);

    // Decoded into the cache, so the sweep can terminate on it rather than replaying to the root.
    CHECK(reopened->snapshot_cache().contains(history.head()));

    auto stats = vdoc::impl::materialize_stats();
    auto const raw = vdoc::impl::materialize(reopened->ops(), head_span, {},
                                             {.cache = &reopened->snapshot_cache(), .stats = &stats});
    CHECK(stats.snapshots_used == 1);
    CHECK(stats.ops_walked == 1);
    CHECK(materialize_through_store(*reopened, history.head()) == expected);
    CHECK(raw.property_count() > 0);
}

INVOCABLE_TEST("vdoc::file - deleting a droppable snapshot costs speed and nothing else", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(8);
    auto const expected = materialize_to_text(history.graph, history.head());
    vdoc::op_id const head_span[] = {history.head()};

    {
        auto const s = open_or_fail(*medium);
        publish_history(*s, history);
        REQUIRE(vdoc::install_snapshot(s->ops(), history.head(), s->snapshot_cache()));
        vdoc::op_id const ops[] = {history.head()};
        REQUIRE(wait_for(s->publish_snapshots(ops)).has_value());
        s->close();
    }

    REQUIRE(medium->delete_first_snapshot());

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().is_empty()); // a droppable snapshot that is simply gone is not even an issue
    CHECK(reopened->snapshots().size() == 0);
    CHECK(materialize_through_store(*reopened, history.head()) == expected);

    auto stats = vdoc::impl::materialize_stats();
    (void)vdoc::impl::materialize(reopened->ops(), head_span, {},
                                  {.cache = &reopened->snapshot_cache(), .stats = &stats});
    CHECK(stats.snapshots_used == 0);
    CHECK(stats.ops_walked == history.ops.size());
}

INVOCABLE_TEST("vdoc::file - a droppable snapshot that will not decode is an issue naming its op",
               (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(8);
    auto const expected = materialize_to_text(history.graph, history.head());

    {
        auto const s = open_or_fail(*medium);
        publish_history(*s, history);
        REQUIRE(vdoc::install_snapshot(s->ops(), history.head(), s->snapshot_cache()));
        vdoc::op_id const ops[] = {history.head()};
        REQUIRE(wait_for(s->publish_snapshots(ops)).has_value());
        s->close();
    }

    REQUIRE(medium->corrupt_first_snapshot_payload());

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().contains(load_issue_kind::missing_snapshot));

    // The issue NAMES the op, which is the contract diagnostics.hh states for this kind.
    auto const* issue = reopened->report().find_first(load_issue_kind::missing_snapshot);
    REQUIRE(issue != nullptr);
    CHECK(issue->op == history.head());

    // The document is untouched: a snapshot is derived, so losing one costs a replay.
    CHECK(materialize_through_store(*reopened, history.head()) == expected);
    CHECK(!reopened->snapshot_cache().contains(history.head()));
}

INVOCABLE_TEST("vdoc::file - a required snapshot that will not decode is a hard failure", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(8);

    {
        auto const s = open_or_fail(*medium);
        publish_history(*s, history);
        REQUIRE(vdoc::install_snapshot(s->ops(), history.head(), s->snapshot_cache()));
        vdoc::op_id const ops[] = {history.head()};
        REQUIRE(wait_for(s->publish_snapshots(ops)).has_value());
        s->close();
    }

    REQUIRE(medium->set_first_snapshot_required(true));
    REQUIRE(medium->corrupt_first_snapshot_payload());

    // Not an issue on an otherwise-open document: what it stood in for is gone, so there is no correct document left.
    CHECK(!medium->open().has_value());
}

INVOCABLE_TEST("vdoc::file - a snapshot under an unknown encoding survives untouched", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(8);

    {
        auto const s = open_or_fail(*medium);
        publish_history(*s, history);
        REQUIRE(vdoc::install_snapshot(s->ops(), history.head(), s->snapshot_cache()));
        vdoc::op_id const ops[] = {history.head()};
        REQUIRE(wait_for(s->publish_snapshots(ops)).has_value());
        s->close();
    }

    REQUIRE(medium->set_first_snapshot_encoding("zstd-from-the-future"));
    auto const before = medium->snapshot_bytes();

    {
        auto const reopened = open_or_fail(*medium);
        CHECK(reopened->report().contains(load_issue_kind::unknown_encoding));
        CHECK(!reopened->snapshot_cache().contains(history.head()));

        // Republishing must not rewrite it: publishing only ever inserts, which is what preserves a newer build's row.
        REQUIRE(wait_for(reopened->publish({.refs = {{cc::string("main"), history.head()}}})).has_value());
        reopened->close();
    }

    CHECK(same_bytes(medium->snapshot_bytes(), before));
}

INVOCABLE_TEST("vdoc::file - a pruned document materializes to what it did before", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(12);
    auto const expected = materialize_to_text(history.graph, history.head());

    {
        auto const s = open_or_fail(*medium);
        publish_history(*s, history);

        auto pruned = wait_for(s->prune(history.head()));
        REQUIRE(pruned.has_value());
        CHECK(pruned.value().ops_skeletonized == history.ops.size() - 1);

        // In the same process, before any reopen — the in-memory graph followed the file.
        CHECK(materialize_through_store(*s, history.head()) == expected);
        s->close();
    }

    CHECK(medium->first_snapshot_is_required());

    // And again after a reopen, which is the leg that proves the ENCODING round-trips rather than just the cache.
    auto const reopened = open_or_fail(*medium);
    CHECK(materialize_through_store(*reopened, history.head()) == expected);
    CHECK(reopened->snapshot_cache().is_pinned(history.head()));
}

INVOCABLE_TEST("vdoc::file - pruning leaves skeletons rather than holes", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(12);

    {
        auto const s = open_or_fail(*medium);
        publish_history(*s, history);
        REQUIRE(wait_for(s->prune(history.head())).has_value());
        s->close();
    }

    auto const reopened = open_or_fail(*medium);

    // Every op is still THERE — only its content is gone, so ancestry through it survives.
    CHECK(reopened->ops().size() == history.ops.size());
    for (isize i = 0; i < history.ops.size() - 1; ++i)
    {
        auto const* const o = reopened->ops().find(history.ops[i]);
        REQUIRE(o != nullptr);
        CHECK(o->is_skeleton());
        CHECK(vdoc::verify_op(*o) == vdoc::op_verification::unverifiable);
    }

    // Routine pruning must never look like tampering, or the one alarm that matters gets ignored.
    CHECK(!reopened->report().contains(load_issue_kind::op_hash_mismatch));
    CHECK(!reopened->report().contains(load_issue_kind::missing_parent));
}

INVOCABLE_TEST("vdoc::file - a pruned op is unverifiable and a corrupt one is a mismatch, in one file",
               (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(12);

    {
        auto const s = open_or_fail(*medium);
        publish_history(*s, history);

        // Prune everything behind op 6, so ops 6..10 keep their payloads and can still be damaged.
        REQUIRE(wait_for(s->prune(history.ops[6])).has_value());
        s->close();
    }

    // corrupt_first_op_payload skips rows with no assignments, so it lands on a surviving op rather than a skeleton.
    REQUIRE(medium->corrupt_first_op_payload());

    auto const reopened = open_or_fail(*medium);

    // Both outcomes appear, and they are DIFFERENT outcomes — which is the whole point of asserting them together.
    CHECK(reopened->report().contains(load_issue_kind::op_hash_mismatch));
    CHECK(reopened->report().count_of(load_issue_kind::op_hash_mismatch) == 1);

    auto skeletons = isize(0);
    for (auto const& id : history.ops)
        if (auto const* const o = reopened->ops().find(id); o != nullptr && o->is_skeleton())
        {
            ++skeletons;
            CHECK(vdoc::verify_op(*o) == vdoc::op_verification::unverifiable);
        }
    CHECK(skeletons == 6);
}

INVOCABLE_TEST("vdoc::file - pruning does not hollow out another ref's history", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto history = make_linear_history(10);

    // A second ref parked on an old op, the way a named version or an open branch would be.
    auto const branch = add_branch(history.graph, history.ops[2], "keep-me");
    auto const branch_expected = materialize_to_text(history.graph, branch);
    auto const main_expected = materialize_to_text(history.graph, history.head());

    auto const s = open_or_fail(*medium);
    copy_ops_into(*s, history.graph, history.ops);
    vdoc::op_id const branch_ops[] = {branch};
    copy_ops_into(*s, history.graph, branch_ops);
    REQUIRE(
        wait_for(s->publish({.refs = {{cc::string("main"), history.head()}, {cc::string("branch"), branch}}})).has_value());

    // REFUSED, and this is the whole behaviour: `branch` forked at op 2, so it does not descend from the head.
    // Pruning anyway would leave the branch offering writers the emptied ops had superseded, and merging the two
    // would fabricate a multi-value nobody authored.
    CHECK(wait_for(s->prune(history.head())).has_error());

    // Refused means nothing was written: both refs still materialize to exactly what they did.
    CHECK(materialize_to_text(s->ops(), branch) == branch_expected);
    CHECK(materialize_to_text(s->ops(), history.head()) == main_expected);
    CHECK(medium->count_snapshots() == 0);
    for (auto const& id : history.ops)
    {
        auto const* const o = s->ops().find(id);
        REQUIRE(o != nullptr);
        CHECK(!o->is_skeleton());
    }

    // Op 2 IS a point every ref descends from, so pruning there is allowed and empties only ops 0 and 1.
    auto const allowed = wait_for(s->prune(history.ops[2]));
    REQUIRE(allowed.has_value());
    CHECK(allowed.value().ops_skeletonized == 2);
}

INVOCABLE_TEST("vdoc::file - pruning is idempotent", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(10);

    auto const s = open_or_fail(*medium);
    publish_history(*s, history);

    auto const first = wait_for(s->prune(history.head()));
    REQUIRE(first.has_value());
    CHECK(first.value().ops_skeletonized == history.ops.size() - 1);

    // Nothing left with a payload behind the head, so the second run empties nothing.
    auto const second = wait_for(s->prune(history.head()));
    REQUIRE(second.has_value());
    CHECK(second.value().ops_skeletonized == 0);
}

INVOCABLE_TEST("vdoc::file - two replicas pruned to different depths agree", (store_impl const& impl))
{
    auto const shallow = impl.make_medium();
    auto const deep = impl.make_medium();
    auto const history = make_linear_history(12);
    auto const expected = materialize_to_text(history.graph, history.head());

    auto const setup = [&](store_medium& medium, isize depth)
    {
        auto const s = open_or_fail(medium);
        publish_history(*s, history);
        REQUIRE(wait_for(s->prune(history.ops[depth])).has_value());
        s->close();
    };

    setup(*shallow, 4);
    setup(*deep, 8);

    auto const a = open_or_fail(*shallow);
    auto const b = open_or_fail(*deep);

    // Neither replica has the ops the other still holds, and both must still say exactly what the un-pruned document
    // said — which only holds THROUGH the snapshot, since replaying alone would read the emptied ops as silent.
    CHECK(materialize_through_store(*a, history.head()) == expected);
    CHECK(materialize_through_store(*b, history.head()) == expected);
    CHECK(materialize_through_store(*a, history.head()) == materialize_through_store(*b, history.head()));
}

INVOCABLE_TEST("vdoc::file - a required snapshot is load-bearing, and replaying without it is lossy",
               (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_linear_history(12);
    auto const expected = materialize_to_text(history.graph, history.head());

    auto const s = open_or_fail(*medium);
    publish_history(*s, history);
    REQUIRE(wait_for(s->prune(history.ops[8])).has_value());

    // Through the snapshot: exactly what it always said.
    CHECK(materialize_through_store(*s, history.head()) == expected);

    // Without it: the emptied ops carry nothing, so their writes are simply not there.
    // This is not a defect — it is what `required` MEANS, and why deleting such a row destroys data.
    CHECK(materialize_to_text(s->ops(), history.head()) != expected);
}
