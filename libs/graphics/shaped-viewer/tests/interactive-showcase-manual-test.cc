#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// The viewer as a caller is meant to use it: `sv::interactive`, real geometry, and a layout of several views.
//
// Everything here is the sanctioned path — no context, no window, no swapchain, no shader library, no render plan is
// touched by hand, and no resource is acquired: an `sv::mesh` carries its own content hashes, so placing one every
// frame uploads nothing after the first.
// The one thing that *is* built outside the loop is the mesh, because that is what pins and hashes.
//
// Controls
//   left-drag            orbit the view under the cursor        wheel        zoom its camera
//   Ctrl+wheel           magnify the image (never the camera)   Ctrl+drag    lift a movable view out of the layout
//
// nx::config::manual keeps it out of the default sweep.
// Run it explicitly:
//   uv run dev.py test "sv - interactive showcase" --manual --timeout 0

TEST("sv - interactive showcase (manual)", nx::config::manual)
{
    // Pinned and hashed once; the loop only ever places these by content.
    auto const cloud_data = sv_test::make_triangle_cloud(64);
    auto const cloud = sv_test::as_mesh("cloud", cloud_data.positions, cloud_data.materials);

    auto const box_data = sv_test::make_cornell_box();
    auto const box = sv_test::as_mesh("cornell box", box_data.positions, box_data.materials);

    auto const key_light = sv::area_light{.center = tg::pos3f(0, 3, 0),
                                          .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                          .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                          .emission = tg::vec3f(12.0f, 12.0f, 12.0f)};

    // A cool-blue SH sky: bright overhead, dimmer below.
    auto const sky = sv::background::gradient(tg::vec3f(0.70f, 0.96f, 1.44f), tg::vec3f(0.21f, 0.28f, 0.37f));

    for (auto f : sv::interactive("shaped-viewer — interactive showcase"))
    {
        // The window's own view is filled with a tree; everything below hangs off it.
        // Each container frames and fills itself in its own hue — a saturated background under a brighter border of the
        // same color — so the nesting is read off the screen rather than inferred.
        auto rows = f.window().view().layout_rows({.border = 2,
                                                   .border_color = tg::vec4f(0.45f, 0.75f, 1.00f, 1),
                                                   .background_color = tg::vec4f(0.10f, 0.30f, 0.60f, 1),
                                                   .padding = 8,
                                                   .spacing = 8});

        // Top row: three views of the same cloud from different angles, each with its own camera and accumulation.
        auto top = rows.columns({.border = 2,
                                 .border_color = tg::vec4f(0.45f, 0.95f, 0.55f, 1),
                                 .background_color = tg::vec4f(0.12f, 0.50f, 0.20f, 1),
                                 .padding = 8,
                                 .spacing = 8});
        for (auto i = 0; i < 3; ++i)
        {
            // One display name, three ids — what the `##` suffix is for.
            auto view = top.add_view("angle##{}", i);
            view.initial_orbit({.target = tg::pos3d(0, 0, 0),
                                .distance = 5.0,
                                .azimuth = tg::angle_d::make_from_degree(40.0 * double(i)),
                                .elevation = tg::angle_d::make_from_degree(20.0)});
            view.movable(); // Ctrl+drag lifts it; the other two then share the row

            auto scene = view.add_scene();
            scene.add_mesh(cloud);
            scene.add_light(key_light);
            scene.background(sky);
        }

        // Bottom row: one view whose layer is *another layout* — its own texture, subdivided again.
        // This is the nesting the flat model could not express: the wrapper renders into a texture the root samples.
        auto inner = rows.add_view("nested").layout_columns({.border = 2,
                                                             .border_color = tg::vec4f(1.00f, 0.55f, 0.25f, 1),
                                                             .background_color = tg::vec4f(0.70f, 0.28f, 0.06f, 1),
                                                             .padding = 8,
                                                             .spacing = 8});
        {
            auto left = inner.add_view("cornell");
            left.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 3.2});

            auto left_scene = left.add_scene();
            left_scene.add_mesh(box);
            left_scene.add_light(key_light);

            // Nearest sampling, so Ctrl+wheel here reads out texels rather than smearing them.
            auto pixels = inner.leaf();
            pixels.sampler(sv::sampler_mode::nearest);
            auto right = pixels.add_view("pixels");
            right.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 4.0});

            // A quarter-resolution view, magnified back up — the case `fit_mode` and the sampler exist for.
            right.resolution(tg::vec2i(160, 120));

            auto right_scene = right.add_scene();
            right_scene.add_mesh(cloud);
            right_scene.add_light(key_light);
        }

        // A wipe between two takes on the same scene: one leaf, two views, one draw.
        // Dragging the split costs nothing — none of it reaches a trace, so neither side restarts.
        auto compare = rows.leaf();
        compare.post_process({.kind = sv::post_process_kind::wipe, .split = 0.5f, .separator_width = 2});
        {
            auto a = compare.add_view("wipe-a");
            a.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 4.0});

            auto a_scene = a.add_scene();
            a_scene.add_mesh(cloud);
            a_scene.add_light(key_light);
            a_scene.background(sv::background::sun(tg::vec3f{0, 1, 0}, tg::vec3f{1, 0, 0})
                                   .combined_with(sv::background::uniform(tg::vec3f(0, 0, 1))));

            auto b = compare.add_view("wipe-b");
            b.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 4.0});

            auto b_scene = b.add_scene();
            b_scene.add_mesh(box);
            b_scene.add_light(key_light);
        }

        // A small inset over everything, out of the flow so its siblings tile as if it were not there.
        // It refreshes at a fifth of the loop's rate: a thumbnail is exactly what a refresh policy is for.
        // Its background is opaque, which is what makes the inset read as a card over the views rather than a hole in
        // them — an out-of-flow node covers whatever it lands on.
        auto overlay = rows.relative({.position = tg::pos2f(0.76f, 0.04f), .size = tg::vec2f(0.2f, 0.2f)},
                                     {.border = 2,
                                      .border_color = tg::vec4f(1.00f, 0.95f, 0.45f, 1),
                                      .background_color = tg::vec4f(0.75f, 0.62f, 0.08f, 1),
                                      .padding = 8});
        auto inset = overlay.add_view("inset");
        inset.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 6.0});
        inset.refresh_rate(0.2f);

        auto inset_scene = inset.add_scene();
        inset_scene.add_mesh(cloud);
        inset_scene.add_light(key_light);
        inset_scene.background(sky);
    }

    CHECK(true); // manual visual test — reaching here means the loop ran and the viewer tore down cleanly
}
