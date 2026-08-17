#include "store_fixture.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <versioned-document/op_builder.hh>

using namespace cc::primitive_defines;
using namespace vdoc::file;
using namespace vdoc::file::test;

namespace
{
/// Opens the medium, requiring the open to succeed.
store_handle open_or_fail(store_medium& medium)
{
    auto opened = medium.open();
    REQUIRE(opened.has_value());
    return cc::move(opened.value());
}

/// Builds the sample history into `s` and publishes it under "main".
vdoc::op_id publish_sample(store& s, sample_history const& history)
{
    copy_ops_into(s, history.graph, history.ops);
    auto published = wait_for(s.publish({.refs = {{cc::string("main"), history.head()}}}));
    REQUIRE(published.has_value());
    return history.head();
}
} // namespace

INVOCABLE_TEST("vdoc::file - a published document round-trips through close and reopen", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();
    auto const expected = materialize_to_text(history.graph, history.head());

    {
        auto const s = open_or_fail(*medium);
        publish_sample(*s, history);
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().is_empty());
    CHECK(reopened->ops().size() == history.ops.size());

    // Materializing to the same text is the whole promise: every op decoded, verified, and in the same DAG shape.
    auto const* head = reopened->refs().get_ptr(cc::string("main"));
    REQUIRE(head != nullptr);
    CHECK(*head == history.head());
    CHECK(materialize_to_text(reopened->ops(), *head) == expected);
}

INVOCABLE_TEST("vdoc::file - an op no ref can reach is not written", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto history = make_sample_history();

    // A discarded drag preview: built, added, and never named by any ref.
    auto const orphan = add_branch(history.graph, history.ops[1], "abandoned");

    {
        auto const s = open_or_fail(*medium);
        copy_ops_into(*s, history.graph, history.ops);
        s->add_op(*history.graph.find(orphan));

        auto const published = wait_for(s->publish({.refs = {{cc::string("main"), history.head()}}}));
        REQUIRE(published.has_value());
        CHECK(published.value().ops_written == history.ops.size()); // the orphan is not among them
        s->close();
    }

    // Asserted by ABSENCE, on the reopened file: the op the caller had in hand simply is not there.
    auto const reopened = open_or_fail(*medium);
    CHECK(!reopened->ops().contains(orphan));
    CHECK(reopened->ops().contains(history.head()));
}

INVOCABLE_TEST("vdoc::file - publishing the same thing twice writes nothing the second time", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    auto const s = open_or_fail(*medium);
    copy_ops_into(*s, history.graph, history.ops);
    auto const changes = publish_changes{.refs = {{cc::string("main"), history.head()}}};

    auto const first = wait_for(s->publish(changes));
    REQUIRE(first.has_value());
    CHECK(first.value().ops_written == history.ops.size());

    auto const after_first = medium->snapshot_bytes();

    auto const second = wait_for(s->publish(changes));
    REQUIRE(second.has_value());
    CHECK(second.value() == publish_result{}); // nothing to write is what idempotence looks like from outside

    auto const after_second = medium->snapshot_bytes();
    CHECK(after_first.size() == after_second.size());
    for (isize i = 0; i < after_first.size(); ++i)
        REQUIRE(after_first[i] == after_second[i]);

    s->close();
}

INVOCABLE_TEST("vdoc::file - a torn op is dropped and reported, and the rest still loads", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    {
        auto const s = open_or_fail(*medium);
        publish_sample(*s, history);
        s->close();
    }

    REQUIRE(medium->corrupt_first_op_structurally());

    // A soft failure NEVER blocks a load: the file opens, says what was wrong, and keeps everything else.
    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->report().contains(load_issue_kind::op_decode_failed));
    CHECK(reopened->ops().size() == history.ops.size() - 1);
}

INVOCABLE_TEST("vdoc::file - a flipped payload byte is a hash mismatch, not a decode failure", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    {
        auto const s = open_or_fail(*medium);
        publish_sample(*s, history);
        s->close();
    }

    REQUIRE(medium->corrupt_first_op_payload());

    auto const reopened = open_or_fail(*medium);

    // The two are never merged: only one of them is a claim about corruption or tampering.
    CHECK(reopened->report().contains(load_issue_kind::op_hash_mismatch));
    CHECK(!reopened->report().contains(load_issue_kind::op_decode_failed));
    CHECK(reopened->ops().size() == history.ops.size() - 1);
}

INVOCABLE_TEST("vdoc::file - a format version from the future fails the open", (store_impl const& impl))
{
    auto const medium = impl.make_medium();

    {
        auto const s = open_or_fail(*medium);
        s->close();
    }

    medium->set_user_version(memory_image::current_user_version + 1);

    // Guessing at a shape this build might misread is worse than refusing, so this one is HARD.
    CHECK(!medium->open().has_value());
}

INVOCABLE_TEST("vdoc::file - unknown tables and columns survive an open-modify-save cycle", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    {
        auto const s = open_or_fail(*medium);
        s->close();
    }

    medium->add_unknown_table("future_state");
    medium->add_unknown_column("ops", "future_flag");

    {
        auto const s = open_or_fail(*medium);

        // Reported, so a caller can see this build did not understand everything.
        auto const* table = s->report().find_first(load_issue_kind::unknown_table);
        auto const* column = s->report().find_first(load_issue_kind::unknown_column);
        REQUIRE(table != nullptr);
        REQUIRE(column != nullptr);
        CHECK(table->name == "future_state");
        CHECK(column->name == "ops.future_flag");

        publish_sample(*s, history);
        s->close();
    }

    // The forward-compatibility promise, tested rather than asserted.
    // A whole open-modify-save cycle ran over both, and they are still here to be reported again — which is what "a
    // rewrite never drops what it did not understand" means from the outside.
    auto const reopened = open_or_fail(*medium);
    auto const* table = reopened->report().find_first(load_issue_kind::unknown_table);
    auto const* column = reopened->report().find_first(load_issue_kind::unknown_column);
    REQUIRE(table != nullptr);
    REQUIRE(column != nullptr);
    CHECK(table->name == "future_state");
    CHECK(column->name == "ops.future_flag");

    // And the document written across that cycle is intact.
    CHECK(reopened->ops().size() == history.ops.size());
    reopened->close();
}

INVOCABLE_TEST("vdoc::file - a workspace write never makes a document look unsaved", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    auto const s = open_or_fail(*medium);
    auto const head = publish_sample(*s, history);
    REQUIRE(s->is_saved(head));

    // Moving a camera must not become edit history, and must not make the document appear unsaved.
    s->set_workspace("viewport/camera", {.version = 1, .value = vdoc::value::of(42)});
    CHECK(s->is_saved(head));
    CHECK(s->refs().size() == 1);

    auto const flushed = wait_for(s->flush_workspace());
    CHECK(flushed.has_value());
    CHECK(s->is_saved(head));

    s->close();
}

INVOCABLE_TEST("vdoc::file - a workspace version this caller does not know reads as absent", (store_impl const& impl))
{
    auto const medium = impl.make_medium();

    {
        auto const s = open_or_fail(*medium);
        s->set_workspace("viewport/camera", {.version = 7, .value = vdoc::value::of("from a newer build")});
        s->close(); // close() flushes, so nothing is lost by forgetting to
    }

    auto const reopened = open_or_fail(*medium);

    // The row is there; a caller asking for a version it does not have simply does not see it.
    CHECK(reopened->workspace().size() == 1);
    CHECK(!reopened->try_get_workspace("viewport/camera", 1).has_value());

    auto const known = reopened->try_get_workspace("viewport/camera", 7);
    REQUIRE(known.has_value());
    CHECK(known.value().as_string() == "from a newer build");

    reopened->close();
}

INVOCABLE_TEST("vdoc::file - flushing writes only the dirty keys", (store_impl const& impl))
{
    auto const medium = impl.make_medium();

    {
        auto const s = open_or_fail(*medium);
        s->set_workspace("a/one", {.version = 1, .value = vdoc::value::of(1)});
        s->set_workspace("b/two", {.version = 1, .value = vdoc::value::of(2)});
        s->close();
    }

    {
        // Only "a/one" is touched, so "b/two" is not in any statement at all — which is what keeps a key a newer
        // build wrote and this one never touched unclobbered.
        auto const s = open_or_fail(*medium);
        s->set_workspace("a/one", {.version = 1, .value = vdoc::value::of(11)});
        auto const flushed = wait_for(s->flush_workspace());
        CHECK(flushed.has_value());
        s->close();
    }

    auto const reopened = open_or_fail(*medium);
    CHECK(reopened->workspace().size() == 2); // the untouched key is still there, at its original value
    auto const one = reopened->try_get_workspace("a/one", 1);
    auto const two = reopened->try_get_workspace("b/two", 1);
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(one.value().as_i64() == 11);
    CHECK(two.value().as_i64() == 2);
    reopened->close();
}

INVOCABLE_TEST("vdoc::file - the first publish failure latches, and a workspace failure does not",
               (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    auto const s = open_or_fail(*medium);
    copy_ops_into(*s, history.graph, history.ops);
    CHECK(s->sticky_error() == nullptr);

    medium->block_writes();

    auto const failed = wait_for(s->publish({.refs = {{cc::string("main"), history.head()}}}));
    CHECK(failed.has_error());

    // Readable immediately, so a failing autosave surfaces long before close.
    auto const* latched = s->sticky_error();
    REQUIRE(latched != nullptr);
    auto const first_message = latched->to_string();

    // A second failure does not replace the first: the latch reports what went wrong, not what went wrong last.
    auto const also_failed = wait_for(s->publish({.refs = {{cc::string("other"), history.head()}}}));
    CHECK(also_failed.has_error());
    REQUIRE(s->sticky_error() != nullptr);
    CHECK(s->sticky_error()->to_string() == first_message);

    s->close();
}

INVOCABLE_TEST("vdoc::file - a failing workspace flush is deliberately not latched", (store_impl const& impl))
{
    auto const medium = impl.make_medium();

    auto const s = open_or_fail(*medium);
    medium->block_writes();

    s->set_workspace("viewport/camera", {.version = 1, .value = vdoc::value::of(1)});
    auto const flushed = wait_for(s->flush_workspace());
    CHECK(flushed.has_error());

    // Losing a camera position is not the data loss the latch exists to report.
    CHECK(s->sticky_error() == nullptr);

    s->close();
}

INVOCABLE_TEST("vdoc::file - a failed publish leaves the medium byte-identical", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    auto const s = open_or_fail(*medium);
    copy_ops_into(*s, history.graph, history.ops);

    auto const before = medium->snapshot_bytes();
    medium->block_writes();

    auto const failed = wait_for(s->publish({.refs = {{cc::string("main"), history.head()}}}));
    CHECK(failed.has_error());

    // One transaction, so there is no partial op set to observe.
    auto const after = medium->snapshot_bytes();
    REQUIRE(before.size() == after.size());
    for (isize i = 0; i < before.size(); ++i)
        REQUIRE(before[i] == after[i]);

    // And the durable set un-claimed what the failure cost, so a retry writes it again rather than skipping it.
    medium->unblock_writes();
    CHECK(!s->is_saved(history.head()));

    auto const retried = wait_for(s->publish({.refs = {{cc::string("main"), history.head()}}}));
    REQUIRE(retried.has_value());
    CHECK(retried.value().ops_written == history.ops.size());

    s->close();
}

INVOCABLE_TEST("vdoc::file - publishing after close fails fast", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const history = make_sample_history();

    auto const s = open_or_fail(*medium);
    copy_ops_into(*s, history.graph, history.ops);
    s->close();

    CHECK(s->is_closed());
    auto const refused = wait_for(s->publish({.refs = {{cc::string("main"), history.head()}}}));
    CHECK(refused.has_error());

    // Not latched: the store is already closed, and the latch exists so a failing autosave surfaces.
    CHECK(s->sticky_error() == nullptr);
}

INVOCABLE_TEST("vdoc::file - close severs the blob source", (store_impl const& impl))
{
    auto const medium = impl.make_medium();
    auto const s = open_or_fail(*medium);

    auto const source = s->make_blob_source();
    CHECK(!source->is_severed());

    s->close();
    CHECK(source->is_severed());

    // After close a blob load completes with an error rather than hanging on a dead handle.
    auto const loaded = wait_for(source->load(blob_hash()));
    CHECK(loaded.has_error());
}
