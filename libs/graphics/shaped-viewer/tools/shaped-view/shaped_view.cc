#include <clean-core/common/utility.hh> // cc::min, cc::max, cc::move
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/ambient.hh> // nx::test_args
#include <nexus/test.hh>
#include <shaped-rendering/input.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// shaped-view: what a double-click on a model opens.
// Every path on the command line is opened, and every file dropped onto the window is placed beside what is already there.
// A file association runs `shaped-view.exe "<path>"`, which the default entry takes whole.

namespace
{
/// One opened file: its load, and where it went once it landed.
struct opened_file
{
    cc::string path;
    sv::asset asset;

    /// Where the row put it; unset until the load has landed.
    cc::optional<tg::vec3f> offset;
};

/// `a` grown to hold `b`.
tg::aabb3f merged(tg::aabb3f const& a, tg::aabb3f const& b)
{
    return tg::aabb3f(tg::pos3f(cc::min(a.min[0], b.min[0]), cc::min(a.min[1], b.min[1]), cc::min(a.min[2], b.min[2])),
                      tg::pos3f(cc::max(a.max[0], b.max[0]), cc::max(a.max[1], b.max[1]), cc::max(a.max[2], b.max[2])));
}

/// The orbit that shows all of `box`, from the front-right and a little above.
sv::orbit_state orbit_around(tg::aabb3f const& box)
{
    auto const center = box.min + (box.max - box.min) * 0.5f;
    auto const radius = f64((box.max - box.min).length()) * 0.5;

    // At the default 60° vertical fov a sphere fits when the eye is twice its radius away; the rest is margin.
    return {.target = tg::pos3d(center[0], center[1], center[2]),
            .distance = cc::max(2.4 * radius, 0.01),
            .azimuth = tg::angle_d::make_from_degree(35.0),
            .elevation = tg::angle_d::make_from_degree(20.0)};
}
} // namespace

// Waives nexus's log rule: a file that does not open or imports with issues is the user's news, never the program's failure.
APP("view", default_entry, allow_logs(cc::rec::level::error))
{
    // Declared before the files, so it outlives every load it started.
    auto const loader = sv::asset_loader();

    auto files = cc::vector<opened_file>();
    auto const open = [&](cc::string_view path)
    {
        if (!sv::asset_format_of_uri(path).has_value())
        {
            CC_LOG_ERROR("shaped-view: '{}' is not a format this viewer reads", path);
            return;
        }
        files.push_back({.path = cc::string(path), .asset = loader.load_async(path)});
    };

    for (auto const arg : nx::test_args())
        open(arg);

    // The row grows along +x: each new file starts where the previous one ended, plus a gap.
    auto row_end = 0.0f;
    auto scene_bounds = cc::optional<tg::aabb3f>();

    for (auto f : sv::interactive("shaped-view"))
    {
        for (auto const& e : f.input_events())
            if (e.is_file_drop())
                open(e.as_file_drop().path);

        auto view = f.window().view();

        auto placed_any = false;
        for (auto& file : files)
        {
            if (file.offset.has_value() || !file.asset.poll())
                continue;

            for (auto const& issue : file.asset.issues())
                CC_LOG_WARNING("shaped-view: {}: {}", file.path, issue);

            auto const box = file.asset.data().bounds();
            if (!box.has_value())
            {
                file.offset = tg::vec3f(0, 0, 0);
                continue;
            }

            // Its left side meets the row's end, it stands on y = 0, and it is centred on z = 0.
            auto const size = box.value().max - box.value().min;
            auto const gap = scene_bounds.has_value() ? 0.25f * cc::max(size[0], size[2]) : 0.0f;
            auto const offset = tg::vec3f(row_end + gap - box.value().min[0], -box.value().min[1],
                                          -(box.value().min[2] + 0.5f * size[2]));
            file.offset = offset;
            row_end += gap + size[0];

            auto const placed = tg::aabb3f(box.value().min + offset, box.value().max + offset);
            scene_bounds = scene_bounds.has_value() ? merged(scene_bounds.value(), placed) : placed;
            placed_any = true;
        }

        for (auto const& file : files)
            if (file.asset.has_error() && !file.offset.has_value())
                CC_LOG_ERROR("shaped-view: could not open '{}': {}", file.path, file.asset.error());
        files.remove_all_where([](opened_file const& file) { return file.asset.has_error(); });

        for (auto const& file : files)
            if (!file.offset.has_value())
                f.hold_capture("a file is still loading");

        // Every arrival reframes, so a dropped file never lands out of sight.
        if (placed_any)
            view.reset_orbit(orbit_around(scene_bounds.value()));

        auto scene = view.add_scene();
        for (auto const& file : files)
        {
            if (!file.offset.has_value())
                continue;
            auto const shift = tg::affine_transform3f::make_translation(file.offset.value());
            for (auto const& mesh : file.asset.meshes())
                scene.add_mesh(mesh).transform(tg::compose(shift, mesh.transform));
        }

        auto const day = sv::daylight();
        scene.background(sv::background::studio());
        scene.add_light("sun", day.sun);
    }
}
