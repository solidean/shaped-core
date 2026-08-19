#pragma once

#include "camera.hh"
#include "cube_components.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <versioned-document-file/store.hh>
#include <versioned-document/document.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/parse.hh>
#include <versioned-document/snapshot_cache.hh>

/// The document side of the editor: a `.vdoc` file, the typed document at the current op, and the history behind it.
///
/// Everything an editor needs from vdoc that is not rendering lives here, so the two EXAMPLE bodies can be about what
/// they demonstrate rather than about plumbing.
///
/// The model in one paragraph: a document IS an op, and nothing is ever edited in place.
/// Every change builds a new op on top of the current one, so editing is always available — including from a revision
/// somewhere back in the history, which simply branches, because a DAG has no opinion about that.

namespace cube_editor
{
class document
{
public:
    /// Opens `path`, creating it if it is not there, and seeds a starter scene into an empty one.
    /// Fails only when SQLite was not compiled in, or the load itself failed.
    [[nodiscard]] static cc::optional<document> open(cc::string_view path);

    document() = default; // empty and unusable; `open` produces a real one, and cc::optional needs this
    document(document&&) = default;
    document& operator=(document&&) = default;
    ~document();

    /// The document at the current op — what to render, and what an edit is relative to.
    [[nodiscard]] vdoc::document const& current() const { return _doc; }

    // -- editing
    //
    // Always available, wherever the timeline is parked.
    // Editing a past revision branches off it; the timeline below then follows the branch, which is what an editor
    // that lets you undo and then do something else has always done.

    void set_placement(vdoc::entity_id entity, placement p, cc::string_view label);
    void set_style(vdoc::entity_id entity, style s, cc::string_view label);
    [[nodiscard]] vdoc::entity_id add_cube(placement p, style s);
    void remove(vdoc::entity_id entity);

    // -- continuous editing (a drag)
    //
    // A drag must show every intermediate state and leave exactly one entry in the history.
    // Between begin and end, each edit is an ordinary op CHAINED onto the last, so vdoc::apply keeps its fast path —
    // fanning them off one parent instead costs a full re-parse per frame (19.9 ms vs 0.009 ms at 8k entities; see
    // versioned-document/docs/concepts/workloads.md, which the edit-latency benchmark pins).
    // On end, one op is built from the state the drag started at to the state it finished at, the chain of
    // intermediates is dropped, and that single op becomes the new current one.

    void begin_continuous_edit();
    [[nodiscard]] bool is_editing_continuously() const { return _edit_origin.has_value(); }

    /// Collapses everything since `begin_continuous_edit` into one op labelled `label`.
    /// A no-op when no drag is open, or when the drag changed nothing.
    void end_continuous_edit(cc::string_view label);

    // -- the timeline
    //
    // There is no linear history in the format: an op DAG is partially ordered, `collect_reachable` returns ops
    // sorted by id BYTES (reproducible, and therefore chronologically meaningless), and `children` is arrival order.
    // A single-user file is a single-parent chain though, so first-parent ancestry IS its history.
    //
    // TODO(versioned-document): `cc::vector<op_id> first_parent_chain(op_id) const` on op_graph would be ~20 lines
    // there and would serve every undo and history UI, instead of each one rebuilding the walk below.

    /// Oldest first; `revision()` indexes it.
    [[nodiscard]] cc::span<vdoc::op_id const> timeline() const { return _timeline; }

    /// Which revision the document is parked on, or -1 while there are none at all.
    [[nodiscard]] int revision() const { return _revision; }
    [[nodiscard]] cc::string_view revision_label(int index) const;

    /// Moves the current op to revision `index`, re-deriving the document there.
    /// A view change and nothing else: it writes no op, and editing from there is as available as anywhere.
    void show_revision(int index);

    // -- saving

    void save();
    [[nodiscard]] bool is_saved() const { return _file->is_saved(_head); }

    // -- workspace: per-user view state, which is NOT part of the document
    //
    // A camera move must not look like an edit — it creates no op, moves no ref, and leaves is_saved alone.

    void store_camera(orbit_camera const& cam);
    [[nodiscard]] cc::optional<orbit_camera> load_camera() const;

    /// One example's imgui layout, under `ui/imgui/<name>`.
    /// Both examples open the same file, so the name is what keeps the editor's panels out of the viewer's.
    void store_ui_settings(cc::string_view name, cc::string_view ini);
    [[nodiscard]] cc::optional<cc::string> load_ui_settings(cc::string_view name) const;

    /// The op DAG behind all of it. For the tests beside this example, which assert on what a drag left in it.
    [[nodiscard]] vdoc::op_graph const& ops() const { return _file->ops(); }

private:
    /// Builds one op over `stage` on top of the current one and moves onto it.
    ///
    /// `transient` marks a drag frame: it stays out of the timeline, and the snapshot is NOT advanced onto it, because
    /// a snapshot may only sit on an op that was accepted as history.
    void commit(vdoc::op_builder&& stage, cc::string_view label, bool transient = false);

    /// Re-derives `_doc` at `_head` from scratch. The correct-and-slow path, for a move the incremental one cannot make.
    void reparse();

    /// Records `_head` as the newest revision, dropping whatever the timeline held past the current one.
    void push_revision(cc::string_view label);

    /// Remembers that an open drag touched `entity`, so the collapse knows what to restate. No-op outside a drag.
    void note_touched(vdoc::entity_id entity);

    void rebuild_timeline();

    /// Everything `entity` currently is, staged onto `stage` — the shape a collapse needs.
    void stage_current_state(vdoc::op_builder& stage, vdoc::entity_id entity) const;

    vdoc::file::store_handle _file;
    vdoc::default_parse_policy _policy;
    vdoc::parse_report _report;

    vdoc::op_id _head;
    vdoc::document _doc;

    cc::vector<vdoc::op_id> _timeline;
    cc::vector<cc::string> _labels;
    int _revision = -1; // an index into _timeline, so -1 while it is empty

    /// Where the open drag started, and which entities it has touched.
    cc::optional<vdoc::op_id> _edit_origin;
    cc::vector<vdoc::entity_id> _touched;
};
} // namespace cube_editor
