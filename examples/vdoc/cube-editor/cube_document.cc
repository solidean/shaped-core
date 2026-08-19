#include "cube_document.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <versioned-document/incremental_parse.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/value_builder.hh>

namespace cube_editor
{
namespace
{
constexpr cc::string_view main_ref = "main";
constexpr cc::string_view camera_key = "viewport/camera";
constexpr i32 camera_version = 1;

/// The op's label, which is what the timeline shows.
/// Metadata is free-form and informational — and still hashed into the op id, so it is part of what is versioned.
[[nodiscard]] vdoc::value label_metadata(cc::string_view label)
{
    return vdoc::value_builder::object().set("label", label).build();
}

[[nodiscard]] cc::string label_of(vdoc::op const* op)
{
    if (op == nullptr)
        return cc::string("(missing op)");

    auto const meta = op->metadata();
    if (meta.kind() != vdoc::value_kind::object)
        return cc::string("(no label)");
    if (auto const label = meta.try_find("label"); label.has_value() && label.value().kind() == vdoc::value_kind::string)
        return cc::string(label.value().as_string());
    return cc::string("(no label)");
}

/// A starter scene, so a fresh file is not an empty screen with nothing to click.
[[nodiscard]] vdoc::op_builder seed_scene()
{
    auto stage = vdoc::op_builder{};
    tg::vec3f const colors[] = {tg::vec3f(0.85f, 0.35f, 0.30f), tg::vec3f(0.35f, 0.75f, 0.45f),
                                tg::vec3f(0.35f, 0.55f, 0.90f)};
    for (auto i = 0; i < 3; ++i)
    {
        auto const entity = vdoc::entity_id::of(cc::format("cube-{}", i));
        stage.set(entity, placement{.center = tg::pos3f(float(i - 1) * 2.5f, 0.0f, 0.0f), .half_extent = 0.8f});
        stage.set(entity, style{.color = colors[i]});
    }
    return stage;
}
} // namespace

document::~document()
{
    if (_file != nullptr)
        _file->close(); // flushes the workspace and drains publishes
}

cc::optional<document> document::open(cc::string_view path)
{
    if (!vdoc::file::store::is_file_storage_available())
        return cc::nullopt;

    auto out = document();
    auto opened = vdoc::file::store::open(path);
    out._file = cc::move(opened.store);

    // A hard load failure rides `loaded`'s error channel; a soft one lands in report() and the file still opened.
    auto pool = cc::async_thread_pool();
    if (auto result = pool.try_blocking_get(opened.loaded); result.has_error())
    {
        cc::eprintln("could not load {}: {}", path, result.error().underlying().to_string());
        return cc::nullopt;
    }

    out._policy = vdoc::default_parse_policy::create_with_registry(registry());

    if (auto const* const head = out._file->refs().get_ptr(main_ref))
        out._head = *head;

    out.reparse();

    // One snapshot pinned after the load, then advanced onto every op accepted as history.
    // That is what keeps an edit ~10 us whatever the document's size: a build and a materialization never walk further
    // back than one op.
    // A file with no ops yet has nothing to snapshot; the first commit installs it instead.
    if (out._file->ops().find(out._head) != nullptr)
        vdoc::install_snapshot(out._file->ops(), out._head, out._file->snapshot_cache());
    out.rebuild_timeline();

    if (out._doc.entities().empty())
    {
        out.commit(seed_scene(), "seed the scene");

        // Waited on, unlike every later save: a publish is fire-and-forget, so a first run closed or killed before it
        // lands would leave a ref pointing at an op whose bytes never arrived — and the next run would then load an
        // empty document and seed a second time.
        (void)pool.try_blocking_get(out._file->publish({.refs = {{cc::string(main_ref), out._head}}}));
    }

    return out;
}

void document::reparse()
{
    auto const raw = _file->ops().materialize(_head, _file->snapshot_cache());
    _report = vdoc::parse_report();
    _doc = vdoc::parse(raw, _policy, _report);
}

void document::commit(vdoc::op_builder&& stage, cc::string_view label, bool transient)
{
    auto const& graph = _file->ops();

    // An empty parent list means "a new document", so the very first op must have one — passing the default op_id
    // would make the root descend from an op that does not exist, and every timeline walk would run off the end.
    auto const has_history = graph.find(_head) != nullptr;
    auto const parents = has_history ? cc::span<vdoc::op_id const>(&_head, 1) : cc::span<vdoc::op_id const>();

    // The cache overload: the diff walk terminates at the pinned snapshot instead of replaying the whole history.
    auto op = cc::move(stage).set_parents(parents).set_metadata(label_metadata(label)).build(graph, _file->snapshot_cache());

    // build() diffs, so re-setting an unchanged property emits nothing — but an op with no assignments is still an op,
    // with its own id and its own place in the DAG. Refusing it here is what keeps "drag it and put it back" out of
    // the history entirely.
    if (has_history && op.assignments().at_end())
        return;

    auto const previous = _head;
    _head = _file->add_op(cc::move(op));

    if (!has_history)
    {
        // The first op has nothing to evolve from, so it is a plain parse — and the snapshot every later edit
        // advances from is installed here.
        this->reparse();
        vdoc::install_snapshot(graph, _head, _file->snapshot_cache());
    }
    else
    {
        // A single-parent child of where the document currently is, which is exactly what the fast path wants — and
        // the reason a drag chains its frames rather than fanning them off one parent.
        auto changes = vdoc::change_summary();
        _doc = vdoc::apply(cc::move(_doc), graph, previous, _head, _policy, _report, changes,
                           {.cache = &_file->snapshot_cache()});

        // Only an op accepted as history may carry the snapshot: a drag frame is about to be dropped, and a snapshot
        // sitting on a dropped op would leave the next materialization with nothing to terminate at.
        if (!transient)
            vdoc::advance_snapshot(graph, _file->snapshot_cache(), previous, _head);
    }

    if (transient)
        return;

    this->push_revision(label);
}

void document::push_revision(cc::string_view label)
{
    // Committing from a past revision abandons whatever followed it — an ordinary undo-then-edit.
    // The abandoned ops stay in the graph, reachable by nothing, which is all "abandoned" means here.
    // `_revision` is -1 while the timeline is empty, so the first revision truncates to nothing rather than growing.
    _timeline.resize_down_to(_revision + 1);
    _labels.resize_down_to(_revision + 1);

    _timeline.push_back(_head);
    _labels.push_back(cc::string(label));
    _revision = int(_timeline.size()) - 1;
}

void document::set_placement(vdoc::entity_id entity, placement p, cc::string_view label)
{
    auto stage = vdoc::op_builder{};
    stage.set(entity, p);
    this->note_touched(entity);
    this->commit(cc::move(stage), label, this->is_editing_continuously());
}

void document::set_style(vdoc::entity_id entity, style s, cc::string_view label)
{
    auto stage = vdoc::op_builder{};
    stage.set(entity, s);
    this->note_touched(entity);
    this->commit(cc::move(stage), label, this->is_editing_continuously());
}

void document::note_touched(vdoc::entity_id entity)
{
    if (!_edit_origin.has_value())
        return;
    for (auto const seen : _touched)
        if (seen == entity)
            return;
    _touched.push_back(entity);
}

vdoc::entity_id document::add_cube(placement p, style s)
{
    // The smallest unused name, so ids stay readable and a delete-then-add reuses the slot.
    auto index = 0;
    while (_doc.has<placement>(vdoc::entity_id::of(cc::format("cube-{}", index))))
        ++index;

    auto const entity = vdoc::entity_id::of(cc::format("cube-{}", index));
    auto stage = vdoc::op_builder{};
    stage.set(entity, p);
    stage.set(entity, s);
    this->commit(cc::move(stage), cc::format("add {}", entity.as_string_view()));
    return entity;
}

void document::remove(vdoc::entity_id entity)
{
    // Deletion is interpretation, not storage: this writes $alive = false and removes nothing.
    // Which is exactly why moving back down the timeline brings the cube straight back.
    auto stage = vdoc::op_builder{};
    stage.remove_entity(entity);
    this->commit(cc::move(stage), cc::format("delete {}", entity.as_string_view()));
}

void document::begin_continuous_edit()
{
    if (_edit_origin.has_value())
        return;
    _edit_origin = _head;
    _touched.clear();
}

void document::stage_current_state(vdoc::op_builder& stage, vdoc::entity_id entity) const
{
    auto const* const p = _doc.get<placement>(entity);
    auto const* const s = _doc.get<style>(entity);

    // An entity the drag removed has neither, and the collapse must say so rather than silently keeping it.
    if (p == nullptr && s == nullptr)
    {
        stage.remove_entity(entity);
        return;
    }
    if (p != nullptr)
        stage.set(entity, *p);
    if (s != nullptr)
        stage.set(entity, *s);
}

void document::end_continuous_edit(cc::string_view label)
{
    if (!_edit_origin.has_value())
        return;

    auto const origin = _edit_origin.value();
    _edit_origin = cc::nullopt;

    if (_touched.empty() || _head == origin)
    {
        _touched.clear();
        return; // the drag never changed anything
    }

    auto const& graph = _file->ops();

    // One op from where the drag started to where it ended.
    // op_builder diffs against its parents, so staging the final state of every touched entity against `origin`
    // yields exactly the net change and nothing in between — which is why the history gets one entry for a drag
    // that produced hundreds of frames.
    auto stage = vdoc::op_builder{};
    for (auto const entity : _touched)
        this->stage_current_state(stage, entity);

    auto op = cc::move(stage)
                  .set_parents(cc::span<vdoc::op_id const>(&origin, 1))
                  .set_metadata(label_metadata(label))
                  .build(graph, _file->snapshot_cache());

    // A drag that ended where it started diffs to nothing against `origin`, and then the whole gesture is simply not
    // history: the frames are dropped and the current op goes back to where the drag began.
    auto const collapsed = op.assignments().at_end() ? origin : _file->add_op(cc::move(op));

    // Forget the frames. They were never history and were never published — nothing descends from them, which is
    // exactly the situation drop_leaf is for. Newest first, since a leaf is all it will remove.
    // `collapsed` is skipped: a drag that ended where its first frame already was produces the very same op.
    for (auto id = _head; id != origin;)
    {
        auto const* const frame = graph.find(id);
        if (frame == nullptr || frame->parents.empty())
            break;
        auto const parent = frame->parents[0];
        if (id != collapsed)
            (void)_file->drop_leaf_op(id);
        id = parent;
    }

    // The collapsed op produces the same document the last frame did, so `_doc` is already the document at it.
    _head = collapsed;
    _touched.clear();

    if (collapsed == origin)
        return; // nothing changed, so there is no revision to record and no snapshot to move

    vdoc::advance_snapshot(graph, _file->snapshot_cache(), origin, _head);
    this->push_revision(label);
}

void document::rebuild_timeline()
{
    _timeline.clear();
    _labels.clear();

    // A file with no `main` ref yet has no head and therefore no timeline — not one revision that does not exist.
    auto const& graph = _file->ops();
    if (graph.find(_head) == nullptr)
    {
        _revision = -1; // no revisions at all, which is not the same as being parked on the first one
        return;
    }

    for (auto id = _head;;)
    {
        auto const* const op = graph.find(id);
        _timeline.push_back(id);
        _labels.push_back(label_of(op));

        if (op == nullptr || op->parents.empty())
            break;
        id = op->parents[0]; // single-user file, so first-parent ancestry IS the history
    }

    // Walked newest-first; the timeline reads oldest-first.
    for (auto i = isize(0), j = _timeline.size() - 1; i < j; ++i, --j)
    {
        cc::swap(_timeline[i], _timeline[j]);
        cc::swap(_labels[i], _labels[j]);
    }
    _revision = int(_timeline.size()) - 1;
}

cc::string_view document::revision_label(int index) const
{
    if (index < 0 || index >= int(_labels.size()))
        return "";
    return _labels[index];
}

void document::show_revision(int index)
{
    if (_timeline.empty())
        return;

    index = cc::clamp(index, 0, int(_timeline.size()) - 1);
    if (index == _revision)
        return;

    _revision = index;
    _head = _timeline[index];

    // Re-materialized and re-parsed rather than applied: vdoc::apply's fast path is forward-only along a single-parent
    // chain, so it cannot serve a move backwards at all, and a long jump forwards falls back to this anyway.
    // Correct in both directions, and the honest cost of scrubbing to an arbitrary point.
    this->reparse();
}

void document::save()
{
    // Ops are derived from the refs by reachability, never listed — an op no ref can reach cannot be published.
    // Fire and forget: a failure latches into sticky_error() rather than blocking the frame.
    (void)_file->publish({.refs = {{cc::string(main_ref), _head}}});
}

void document::store_camera(orbit_camera const& cam)
{
    auto const value = vdoc::value_builder::object()
                           .set("target_x", cam.target[0])
                           .set("target_y", cam.target[1])
                           .set("target_z", cam.target[2])
                           .set("distance", cam.distance)
                           .set("yaw_deg", cam.yaw.degree())
                           .set("pitch_deg", cam.pitch.degree())
                           .build();
    _file->set_workspace(camera_key, {.version = camera_version, .value = value});
}

cc::optional<orbit_camera> document::load_camera() const
{
    // The caller names the version it can handle, because the store cannot know an application's versions.
    // A row written under any other version reads as absent and is left in the table for the build that wrote it.
    auto const stored = _file->try_get_workspace(camera_key, camera_version);
    if (!stored.has_value() || stored.value().kind() != vdoc::value_kind::object)
        return cc::nullopt;

    auto const& v = stored.value();
    auto const number = [&](cc::string_view key, float fallback)
    {
        auto const found = v.try_find(key);
        return found.has_value() ? f32(found.value().as_f64()) : fallback;
    };

    auto cam = orbit_camera();
    cam.target = tg::pos3f(number("target_x", 0), number("target_y", 0), number("target_z", 0));
    cam.distance = number("distance", 12.0f);
    cam.yaw = tg::angle_f::make_from_degree(number("yaw_deg", 35.0f));
    cam.pitch = tg::angle_f::make_from_degree(number("pitch_deg", 28.0f));
    return cam;
}

vdoc::component_registry const& registry()
{
    static auto const the_registry = []
    {
        auto r = vdoc::component_registry();
        r.register_component<placement>();
        r.register_component<style>();
        return r;
    }();
    return the_registry;
}
} // namespace cube_editor
