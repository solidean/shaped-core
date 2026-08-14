#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// The fluent authoring API, end to end, in a real window.
//
// `sv::interactive` owns the viewer for the loop's lifetime, so nothing here holds one — and no context is threaded
// through either: the viewer acquires one through `sv::acquire_context`, which is unset here so the built-in default
// answers.
// These tests deliberately bring up nothing of their own, because that is the path a caller takes.
//
// A frame carries the whole window surface, which carries the whole view surface, so `f.add_scene()` and
// `f.window().view().add_scene()` are the same call — which is what the second test below shows.
//
// Controls: left-drag orbits the view under the cursor and wheel zooms its camera.
// Ctrl+wheel magnifies the image itself — a readout of the converged pixels rather than a camera move, so it never
// restarts an accumulation.
// Ctrl+left-drag lifts a view the caller offered out of the layout and floats it over the window.
//
// nx::config::manual keeps these out of the default sweep.
// Run them explicitly:
//   uv run dev.py test "sv - interactive" --manual --timeout 0

TEST("sv - interactive viewer, a layout of views (manual)", nx::config::manual)
{
    // The very context the viewer is about to use, so probing it costs no second device.
    auto ctx_r = sv::acquire_viewer_context();
    if (ctx_r.has_error())
        SKIP("no rendering context available");

    {
        auto probe = ctx_r.value()->create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx_r.value()->drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto frames = 0;
    for (auto f : sv::interactive("layout demo", {.title = "shaped-viewer — layout of views"}))
    {
        // A row of panes across the window, with a frame around the whole thing.
        auto rows = f.window().view().layout_rows({.padding = 8, .spacing = 6});

        // The top row is split into two columns, each its own view with its own camera and its own accumulation.
        auto top = rows.columns({.spacing = 6});
        for (auto i = 0; i < 2; ++i)
        {
            auto const id = f.scoped_id(i); // the same name under two scopes names two views
            auto view = top.add_view("pane");
            view.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 5.0 + double(i)});

            // Ctrl+left-drag lifts either of these out of the row; the other then takes the whole width.
            view.movable();
            view.add_scene().add_light({.center = tg::pos3f(0, 3, 0),
                                        .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                        .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                        .emission = tg::vec3f(12.0f, 12.0f, 12.0f)});
        }

        // The bottom row is a single view holding a *nested* layout — its own texture, subdivided again.
        // Ctrl+wheel here is what proves the routing: the hit has to reach "a" or "b", not the wrapper holding them.
        auto inner
            = rows.add_view("bottom").layout_columns({.border = 2, .border_color = tg::vec4f(0.9f, 0.3f, 0.1f, 1)});
        (void)inner.add_view("a");

        // Nearest sampling, so magnifying this one reads out texels rather than smearing them.
        auto pixels = inner.leaf().sampler(sv::sampler_mode::nearest);
        (void)pixels.add_view("b");

        // A small inset over the whole thing, out of the flow so its siblings tile as if it were absent.
        auto overlay = rows.relative({.position = tg::pos2f(0.72f, 0.06f), .size = tg::vec2f(0.24f, 0.24f)});
        (void)overlay.add_view("inset");

        if (++frames > 100000)
            break;
    }

    CHECK(true); // manual visual test — reaching here means the loop ran and the viewer tore down cleanly
}

// The manual loop: the same frame, the same authoring calls, ended by hand instead of by scope.
// This is the path an application takes whose own loop must stay in charge, so nothing here is a range.
TEST("sv - interactive viewer, the manual begin_frame/end_frame loop (manual)", nx::config::manual)
{
    auto ctx_r = sv::acquire_viewer_context();
    if (ctx_r.has_error())
        SKIP("no rendering context available");

    {
        auto probe = ctx_r.value()->create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx_r.value()->drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto viewer = sv::viewer::create("raw loop demo", {.title = "shaped-viewer — raw begin_frame/end_frame"});

    auto frames = 0;
    while (viewer.is_running())
    {
        auto& f = viewer.begin_frame();
        if (!f)
            continue; // the window cannot draw right now — minimized, or closing; a closed frame needs no end_frame

        auto rows = f.window().view().layout_rows({.padding = 8, .spacing = 6});
        rows.add_view("left").initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 5.0});
        rows.add_view("right").add_scene().add_light({.center = tg::pos3f(0, 3, 0),
                                                      .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                                      .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                                      .emission = tg::vec3f(12.0f, 12.0f, 12.0f)});

        // A manual frame presents here and nowhere else — the viewer owns it until this call.
        viewer.end_frame();

        if (++frames > 100000)
            break;
    }

    CHECK(frames >= 0);
}

TEST("sv - interactive viewer, one scene with no ceremony (manual)", nx::config::manual)
{
    // The very context the viewer is about to use, so probing it costs no second device.
    auto ctx_r = sv::acquire_viewer_context();
    if (ctx_r.has_error())
        SKIP("no rendering context available");

    {
        auto probe = ctx_r.value()->create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx_r.value()->drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    // The shortest thing that renders: no window, no view, no layout named at all — and no title either, so the
    // viewer's own name titles the window.
    // `f.add_scene()` resolves to the default window's default view's 3D layer, and that view fills the window.
    for (auto f : sv::interactive("shaped-viewer — one scene"))
    {
        f.add_scene().add_light({.center = tg::pos3f(0, 3, 0),
                                 .half_extent_u = tg::vec3f(0.75f, 0, 0),
                                 .half_extent_v = tg::vec3f(0, 0, 0.75f),
                                 .emission = tg::vec3f(14.0f, 14.0f, 14.0f)});
    }

    CHECK(true);
}
