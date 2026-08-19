#include "cube_document.hh"

#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/test.hh>

// Ordinary tests, in an example binary.
//
// That is the case a *-example target is allowed to carry: the document layer this example grew is small but it is
// where the continuous-edit rule actually lives, and a rule stated only in a comment is one nobody notices breaking.
// `dev.py test` runs these; the EXAMPLEs beside them are in another bucket and never run here.

namespace
{
using namespace cube_editor;

/// The process-wide default async pool the store needs.
///
/// A `.vdoc` store completes async nodes on its own actor thread, and a completion off a worker has to route
/// somewhere — without an installed default it asserts. The application installs one at startup (cube_app does);
/// these tests install one lazily and share it, because a default may only be installed once per process.
void ensure_default_async_pool()
{
    static cc::async_thread_pool pool;
    static cc::scoped_default_async_pool const installed(pool);
}

/// Opens a document on a private file, with everything a store needs already standing.
[[nodiscard]] cc::optional<document> open_scratch_document();

/// A private file per test, removed on the way in rather than the way out, so a crashed run leaves evidence.
[[nodiscard]] cc::string scratch_path()
{
    static cc::atomic<i64> counter = {0};
    auto const path = cc::format("{}/cube-editor-test-{}.vdoc", cc::temp_directory_path(),
                                 counter.fetch_add(1, cc::memory_order_relaxed));
    cc::remove_file(path);
    cc::remove_file(cc::format("{}-wal", path));
    cc::remove_file(cc::format("{}-shm", path));
    return path;
}

cc::optional<document> open_scratch_document()
{
    ensure_default_async_pool();
    return document::open(scratch_path());
}

[[nodiscard]] placement at(float x) { return {.center = tg::pos3f(x, 0, 0), .half_extent = 0.8f}; }
} // namespace

TEST("cube-editor - a drag is one revision, however many frames it took", nx::config::exclusive("vdoc-file-store"))
{
    if (!vdoc::file::store::is_file_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto doc = open_scratch_document();
    REQUIRE(doc.has_value());

    auto const entity = vdoc::entity_id::of("cube-0");
    auto const revisions_before = doc.value().timeline().size();
    auto const leaves_before = doc.value().ops().leaves().size();

    doc.value().begin_continuous_edit();
    for (auto frame = 0; frame < 32; ++frame)
        doc.value().set_placement(entity, at(float(frame)), "move");

    // Every frame is visible while the drag is open — that is the whole point of writing them at all.
    CHECK(doc.value().current().get<placement>(entity)->center[0] == 31.0f);
    CHECK(doc.value().timeline().size() == revisions_before); // ...and none of them is history yet

    doc.value().end_continuous_edit("move cube-0");

    CHECK(doc.value().timeline().size() == revisions_before + 1);
    CHECK(doc.value().revision() == int(revisions_before));
    CHECK(doc.value().current().get<placement>(entity)->center[0] == 31.0f);

    // The frames are gone rather than merely unreferenced: a session that kept them would grow without bound.
    CHECK(doc.value().ops().leaves().size() == leaves_before);
}

TEST("cube-editor - a drag that ends where it started writes nothing", nx::config::exclusive("vdoc-file-store"))
{
    if (!vdoc::file::store::is_file_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto doc = open_scratch_document();
    REQUIRE(doc.has_value());

    auto const entity = vdoc::entity_id::of("cube-0");
    auto const original = *doc.value().current().get<placement>(entity);
    auto const revisions_before = doc.value().timeline().size();

    doc.value().begin_continuous_edit();
    doc.value().set_placement(entity, at(5.0f), "move");
    doc.value().set_placement(entity, original, "move");
    doc.value().end_continuous_edit("move cube-0");

    // op_builder diffs against the parent, so a net change of nothing produces no assignments and no op.
    CHECK(doc.value().timeline().size() == revisions_before);
    CHECK(doc.value().current().get<placement>(entity)->center == original.center);
}

TEST("cube-editor - editing a past revision branches off it", nx::config::exclusive("vdoc-file-store"))
{
    if (!vdoc::file::store::is_file_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto doc = open_scratch_document();
    REQUIRE(doc.has_value());

    auto const entity = vdoc::entity_id::of("cube-0");
    doc.value().set_placement(entity, at(1.0f), "move to 1");
    doc.value().set_placement(entity, at(2.0f), "move to 2");
    auto const tip = int(doc.value().timeline().size()) - 1;

    // Back one revision, which is a view change and writes nothing.
    doc.value().show_revision(tip - 1);
    CHECK(doc.value().current().get<placement>(entity)->center[0] == 1.0f);
    CHECK(doc.value().timeline().size() == isize(tip) + 1);

    // Editing from there is ordinary, and what followed is simply left behind.
    doc.value().set_placement(entity, at(9.0f), "move to 9");
    CHECK(doc.value().current().get<placement>(entity)->center[0] == 9.0f);
    CHECK(doc.value().timeline().size() == isize(tip) + 1);
    CHECK(doc.value().revision() == tip);
    CHECK(doc.value().revision_label(tip) == "move to 9");
}

TEST("cube-editor - an imgui layout round-trips per example and writes no history", nx::config::exclusive("vdoc-file-store"))
{
    if (!vdoc::file::store::is_file_storage_available())
        SKIP("no SQLite backend was compiled in");

    ensure_default_async_pool();
    auto const path = scratch_path();

    {
        auto doc = document::open(path);
        REQUIRE(doc.has_value());

        auto const revisions_before = doc.value().timeline().size();
        doc.value().store_ui_settings("editor", "[Window][cube editor]\nPos=20,20\n");
        doc.value().store_ui_settings("viewer", "[Window][cube viewer]\nPos=400,300\n");

        // The whole reason the layout lives in the workspace: it is not an edit.
        CHECK(doc.value().timeline().size() == revisions_before);
    } // close() flushes the workspace

    auto reopened = document::open(path);
    REQUIRE(reopened.has_value());

    auto const editor = reopened.value().load_ui_settings("editor");
    auto const viewer = reopened.value().load_ui_settings("viewer");
    REQUIRE(editor.has_value());
    REQUIRE(viewer.has_value());

    // Both examples open the same file, so what matters is that neither name read the other's layout back.
    CHECK(editor.value() == "[Window][cube editor]\nPos=20,20\n");
    CHECK(viewer.value() == "[Window][cube viewer]\nPos=400,300\n");

    CHECK(!reopened.value().load_ui_settings("never-written").has_value());
}

TEST("cube-editor - deleting an entity is reversible by moving back", nx::config::exclusive("vdoc-file-store"))
{
    if (!vdoc::file::store::is_file_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto doc = open_scratch_document();
    REQUIRE(doc.has_value());

    auto const entity = vdoc::entity_id::of("cube-0");
    REQUIRE(doc.value().current().has<placement>(entity));

    doc.value().remove(entity);
    CHECK(!doc.value().current().has<placement>(entity));

    // Nothing was removed from storage — $alive was written false, and the revision before it still says otherwise.
    doc.value().show_revision(doc.value().revision() - 1);
    CHECK(doc.value().current().has<placement>(entity));
}
