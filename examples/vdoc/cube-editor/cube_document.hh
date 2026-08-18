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

/// The document side of the editor: a `.vdoc` file, the typed document at its head, and the history behind it.
///
/// Everything an editor needs from vdoc that is not rendering lives here, so the two EXAMPLE bodies can be about
/// what they demonstrate rather than about plumbing.

namespace cube_editor
{
class document
{
public:
    /// Opens `path`, creating it if it is not there, and seeds a starter scene into an empty one.
    /// Fails only when SQLite was not compiled in, or the load itself failed.
    [[nodiscard]] static cc::optional<document> open(cc::string_view path);

    document(document&&) = default;
    document& operator=(document&&) = default;
    ~document();

    /// What to render: the head document, or the revision the history slider is parked on.
    [[nodiscard]] vdoc::document const& visible() const { return _preview.has_value() ? _preview.value() : _doc; }

    // -- editing; all no-ops while a past revision is being previewed, because history is immutable

    [[nodiscard]] bool is_editable() const { return !_preview.has_value(); }

    void set_placement(vdoc::entity_id entity, placement p, cc::string_view label);
    void set_style(vdoc::entity_id entity, style s, cc::string_view label);
    [[nodiscard]] vdoc::entity_id add_cube(placement p, style s);
    void remove(vdoc::entity_id entity);

    // -- history
    //
    // There is no linear history in the format: an op DAG is partially ordered, `collect_reachable` returns ops
    // sorted by id BYTES (reproducible, and therefore chronologically meaningless), and `children` is arrival order.
    // A single-user file is a single-parent chain though, so first-parent ancestry IS its history.
    //
    // TODO(versioned-document): `cc::vector<op_id> first_parent_chain(op_id) const` on op_graph would be ~20 lines
    // there and would serve every undo and history UI, instead of each one rebuilding the walk below.

    /// Oldest first. The last entry is always the head.
    [[nodiscard]] cc::span<vdoc::op_id const> history() const { return _history; }
    [[nodiscard]] int revision() const { return _revision; }
    [[nodiscard]] cc::string_view revision_label(int index) const;

    /// Parks the view on revision `index`; the last index returns to the head and re-enables editing.
    void show_revision(int index);

    // -- saving

    void save();
    [[nodiscard]] bool is_saved() const { return _file->is_saved(_head); }

    // -- workspace: per-user view state, which is NOT part of the document
    //
    // A camera move must not look like an edit — it creates no op, moves no ref, and leaves is_saved alone.

    void store_camera(orbit_camera const& cam);
    [[nodiscard]] cc::optional<orbit_camera> load_camera() const;

public:
    /// Empty and unusable; `open` is what produces a real one.
    /// Public because cc::optional needs to be able to default-construct its storage.
    document() = default;

private:

    /// Builds one op over `stage`, adds it as the new head, and evolves the typed document onto it.
    /// This is the whole realtime edit path: build against the snapshot, advance it, apply incrementally.
    void commit(vdoc::op_builder&& stage, cc::string_view label);

    void rebuild_history();

    vdoc::file::store_handle _file;
    vdoc::component_registry _registry;
    vdoc::default_parse_policy _policy;
    vdoc::snapshot_cache _cache;
    vdoc::parse_report _report;

    vdoc::op_id _head;
    vdoc::document _doc;

    cc::vector<vdoc::op_id> _history;
    cc::vector<cc::string> _labels;
    int _revision = 0;
    cc::optional<vdoc::document> _preview;
    vdoc::parse_report _preview_report;
};
} // namespace cube_editor
