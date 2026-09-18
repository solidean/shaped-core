#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

// Every kind of light the viewer has, one per panel, over the same small stage — so what differs between two panels is
// the light and nothing else.
//
//   point        a bulb: one shadow ray, falling off with the inverse square, sharp shadows
//   spot         the same point with a cone, which is a shaping on it rather than a kind of its own
//   softbox      a rect emitting from both faces, and seen by the camera — the panel is in frame
//   directional  parallel light: no position, no falloff, shadows as sharp as a point's
//   sun          a disc at infinity, widened far past the real sun's half degree so its penumbra is visible
//   daylight     `sv::daylight()`: a sky and the sun that lights it, handed back as a pair so the sun counts once
//
// Every intensity is in its path's own unit — candela, nits, lux — and each is chosen to put about the same
// illuminance on the floor under the stage, so the panels compare by shape rather than by brightness.
// The dim studio sky in the first five is only there so the unlit side of each object still reads.
// docs/lights.md has the design behind all of it.
//
// Controls, per panel
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/lights
//   uv run dev.py example shaped-viewer/lights --capture    # headless, writes an image

namespace
{
/// Two triangles of a square floor of side `size` at y = 0.
cc::vector<tg::pos3f> floor_triangles(float size)
{
    auto const h = size * 0.5f;
    auto const a = tg::pos3f(-h, 0, -h);
    auto const b = tg::pos3f(h, 0, -h);
    auto const c = tg::pos3f(h, 0, h);
    auto const d = tg::pos3f(-h, 0, h);
    return {a, c, b, a, d, c};
}

/// The 12 triangles of an axis-aligned box from `lo` to `hi`, wound so every face's normal points outward.
cc::vector<tg::pos3f> box_triangles(tg::pos3f lo, tg::pos3f hi)
{
    // Corner i: bit 0 is +x, bit 1 is +y, bit 2 is +z.
    auto const corner
        = [&](int i) { return tg::pos3f((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]); };

    int const quads[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};

    auto out = cc::vector<tg::pos3f>();
    for (auto const& q : quads)
        for (auto const k : {0, 1, 2, 0, 2, 3})
            out.push_back(corner(q[k]));
    return out;
}

/// One mesh with a color per triangle: a light floor, and a warm box standing on it.
sv::mesh make_stage()
{
    auto positions = floor_triangles(8.0f);
    auto colors = cc::vector<tg::vec3f>::create_filled(positions.size() / 3, tg::vec3f(0.75f, 0.75f, 0.72f));

    for (auto const& p : box_triangles(tg::pos3f(-1.4f, 0, -0.5f), tg::pos3f(-0.4f, 1.2f, 0.5f)))
        positions.push_back(p);
    while (colors.size() < positions.size() / 3)
        colors.push_back(tg::vec3f(0.85f, 0.45f, 0.25f));

    return sv::mesh{
        .name = "stage",
        .geometry = sv::triangle_geometry::create_from_positions(positions),
        .attributes = {sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, colors)}};
}
} // namespace

EXAMPLE("shaped-viewer/lights")
{
    using namespace tg::literals;

    // Built once, so its buffers are pinned and hashed; placing it in six views uploads it once.
    auto const stage = make_stage();

    // The dim fill every panel but the daylight one sits in.
    auto const studio = sv::background::studio().scaled(0.12f);

    // Low, about 28 degrees above the horizon, so shadows run long — which is what makes a sun's penumbra visible, since it
    // widens with the distance from the object casting it.
    auto const slant = tg::vec3f(0.85f, -0.55f, 0.45f);

    for (auto f : sv::interactive("shaped-viewer/lights"))
    {
        auto grid = f.window().view().layout_grid(3, 2, {.padding = 6, .spacing = 6});

        // The same stage in every panel; only its light differs.
        auto const panel = [&](cc::string_view id)
        {
            auto view = grid.add_view(id);
            view.initial_orbit({.target = tg::pos3d(0, 0.6, 0),
                                .distance = 7.0,
                                .azimuth = tg::angle_d::make_from_degree(20.0),
                                .elevation = tg::angle_d::make_from_degree(28.0)});

            auto scene = view.add_scene();
            scene.add_mesh(stage);
            scene.add_sphere(tg::sphere3f(tg::pos3f(0.9f, 0.6f, 0.2f), 0.6f));
            return scene;
        };

        // A bulb 2.2 above the stage: about 2 lux on the floor under it is 2 * 2.2^2 candela.
        auto point = panel("point");
        point.background(studio);
        point.add_point_light("bulb", tg::pos3f(-0.2f, 2.2f, -0.4f)).candela(10);

        // A spot straight down, full inside 18 degrees and gone by 28: the pool on the floor is its cone.
        auto spot = panel("spot");
        spot.background(studio);
        spot.add_spot_light("spot", tg::pos3f(0, 3.2f, 0), tg::vec3f(0, -1, 0), 28_deg_f, 18_deg_f).candela(40);

        // A rect hanging in view, visible to the camera as the panel it is.
        // The camera looks down on it, so it emits from both faces: the lower one lights the stage, the upper one is seen.
        // A spread would narrow both, and from up here would hide the panel it is meant to show.
        auto softbox = panel("softbox");
        softbox.background(studio);
        softbox.add_rect_light("softbox", tg::pos3f(0, 2.2f, 0.3f), tg::vec3f(0.8f, 0, 0), tg::vec3f(0, 0, 0.5f))
            .nits(10)
            .face(sv::light_face::both)
            .visible_to_camera();

        auto directional = panel("directional");
        directional.background(studio);
        directional.add_directional_light("sun", slant).lux(2);

        // Ten degrees across rather than half of one, so the penumbra is wide enough to see at this size.
        auto sun = panel("sun");
        sun.background(studio);
        sun.add_sun_light("sun", slant, 10_deg_f).lux(2);

        // The preset hands back a sky with no sun in it and the sun beside it: set both, or half the daylight is missing.
        auto const day = sv::daylight();
        auto daylight = panel("daylight");
        daylight.background(day.sky);
        daylight.add_light("sun", day.sun);
    }
}
