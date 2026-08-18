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

/// The op's label, which is what the history slider shows.
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

    out._registry = make_registry();
    out._policy = vdoc::default_parse_policy::create_with_registry(out._registry);

    if (auto const* const head = out._file->refs().get_ptr(main_ref))
        out._head = *head;

    auto const raw = out._file->ops().materialize(out._head, out._file->snapshot_cache());
    out._doc = vdoc::parse(raw, out._policy, out._report);

    // One snapshot pinned after the load, then advanced on every accepted op.
    // That is what keeps an edit ~10 us whatever the document's size: a build and a materialization never walk
    // further back than one op.
    // A file with no ops yet has nothing to snapshot; the first commit installs it instead.
    if (out._file->ops().find(out._head) != nullptr)
        vdoc::install_snapshot(out._file->ops(), out._head, out._file->snapshot_cache());
    out.rebuild_history();

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

void document::commit(vdoc::op_builder&& stage, cc::string_view label)
{
    CC_ASSERT(this->is_editable(), "an edit while a past revision is shown would fork history behind the user's back");

    auto const& graph = _file->ops();

    // An empty parent list means "a new document", so the very first op must have one — passing the default op_id
    // would make the root descend from an op that does not exist, and every history walk would then run off the end.
    auto const has_history = graph.find(_head) != nullptr;
    auto const parents = has_history ? cc::span<vdoc::op_id const>(&_head, 1) : cc::span<vdoc::op_id const>();

    // The cache overload: the diff walk terminates at the pinned snapshot instead of replaying the whole history.
    auto op = cc::move(stage).set_parents(parents).set_metadata(label_metadata(label)).build(graph, _file->snapshot_cache());

    auto const previous = _head;
    _head = _file->add_op(cc::move(op));
    if (has_history && _head == previous)
        return; // the edit changed nothing, so the builder emitted no assignments and add() collapsed it

    if (has_history)
    {
        vdoc::advance_snapshot(graph, _file->snapshot_cache(), previous, _head);

        auto changes = vdoc::change_summary();
        _doc = vdoc::apply(cc::move(_doc), graph, previous, _head, _policy, _report, changes,
                           {.cache = &_file->snapshot_cache()});
    }
    else
    {
        // The first op has nothing to evolve from, so it is a plain parse — and the snapshot the edit loop advances
        // from every frame after this one is installed here.
        auto const raw = graph.materialize(_head, _file->snapshot_cache());
        _doc = vdoc::parse(raw, _policy, _report);
        vdoc::install_snapshot(graph, _head, _file->snapshot_cache());
    }

    _history.push_back(_head);
    _labels.push_back(cc::string(label));
    _revision = int(_history.size()) - 1;
}

void document::set_placement(vdoc::entity_id entity, placement p, cc::string_view label)
{
    auto stage = vdoc::op_builder{};
    stage.set(entity, p);
    this->commit(cc::move(stage), label);
}

void document::set_style(vdoc::entity_id entity, style s, cc::string_view label)
{
    auto stage = vdoc::op_builder{};
    stage.set(entity, s);
    this->commit(cc::move(stage), label);
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
    // Which is exactly why scrubbing back through the history brings the cube straight back.
    auto stage = vdoc::op_builder{};
    stage.remove_entity(entity);
    this->commit(cc::move(stage), cc::format("delete {}", entity.as_string_view()));
}

void document::rebuild_history()
{
    _history.clear();
    _labels.clear();

    // A file with no `main` ref yet has no head and therefore no history — not one revision that does not exist.
    auto const& graph = _file->ops();
    if (graph.find(_head) == nullptr)
    {
        _revision = 0;
        return;
    }

    for (auto id = _head;;)
    {
        auto const* const op = graph.find(id);
        _history.push_back(id);
        _labels.push_back(label_of(op));

        if (op == nullptr || op->parents.empty())
            break;
        id = op->parents[0]; // single-user file, so first-parent ancestry IS the history
    }

    // Walked newest-first; the slider reads oldest-first.
    for (auto i = isize(0), j = _history.size() - 1; i < j; ++i, --j)
    {
        cc::swap(_history[i], _history[j]);
        cc::swap(_labels[i], _labels[j]);
    }
    _revision = int(_history.size()) - 1;
}

cc::string_view document::revision_label(int index) const
{
    if (index < 0 || index >= int(_labels.size()))
        return "";
    return _labels[index];
}

void document::show_revision(int index)
{
    index = cc::clamp(index, 0, int(_history.size()) - 1);
    _revision = index;

    if (index == int(_history.size()) - 1)
    {
        _preview = cc::nullopt; // back at the head, so editing is live again
        return;
    }

    // An arbitrary jump re-materializes and re-parses: vdoc::apply's fast path is forward-only along a single-parent
    // chain within max_chain_ops.
    // Fine for scrubbing an example's history, and NOT the pattern for a real editor's undo stack, which walks one op
    // at a time and stays on that fast path.
    auto const raw = _file->ops().materialize(_history[index], _file->snapshot_cache());
    _preview_report = vdoc::parse_report();
    _preview = vdoc::parse(raw, _policy, _preview_report);
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

vdoc::component_registry make_registry()
{
    auto registry = vdoc::component_registry();
    registry.register_component<placement>();
    registry.register_component<style>();
    return registry;
}
} // namespace cube_editor
