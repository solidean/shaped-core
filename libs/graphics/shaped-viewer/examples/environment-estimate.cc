#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-rendering/input.hh>
#include <shaped-viewer/all.hh>

// The environment's light, sampled against taken whole where nothing blocks it: the same moving cube in two panels.
//
// The scene is lit by the studio sky alone, and the cube moves, so every frame restarts the image and shows one sample per pixel.
// Left, `sv::environment_estimate::sampled`: each hit stands one sky direction in for the whole sky, so the faces swim with noise.
// Its specular layer reflects the sky only on the rare bounce that lands in its lobe, which is the bright speckles.
// Right, `exact`: the unshadowed light is taken whole, the diffuse half from the sky's irradiance and the specular half from its own lobe, and only what is blocked is sampled.
// What noise is left is the occlusion: the shadow under the cube, the far floor's rare sample the cube blocks, and the cube's sides, whose lower half sees the floor.
// Nothing denoises either panel, so what you see is the estimate itself.
//
// Controls
//   space        pause or resume the cube
//   D            denoise both panels with SVGF, to see what each leaves the denoiser to do
//   left-drag    orbit (per panel)          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/environment-estimate
//   uv run dev.py example shaped-viewer/environment-estimate --capture    # headless, writes an image

namespace
{
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

/// A mesh of `positions` in one matte color.
sv::mesh matte(cc::string_view name, cc::vector<tg::pos3f> const& positions, tg::vec3f color)
{
    auto const colors = cc::vector<tg::vec3f>::create_filled(positions.size() / 3, color);
    return sv::mesh{
        .name = cc::string(name),
        .geometry = sv::triangle_geometry::create_from_positions(positions),
        .attributes = {sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, colors)}};
}
} // namespace

EXAMPLE("shaped-viewer/environment-estimate")
{
    auto const floor
        = matte("floor", box_triangles(tg::pos3f(-4, -0.05f, -4), tg::pos3f(4, 0, 4)), tg::vec3f(0.7f, 0.7f, 0.7f));
    auto const cube = matte("cube", box_triangles(tg::pos3f(-0.5f, -0.5f, -0.5f), tg::pos3f(0.5f, 0.5f, 0.5f)),
                            tg::vec3f(0.8f, 0.45f, 0.3f));

    auto paused = false;
    auto denoised = false;
    // The cube's own clock, which stops while paused.
    auto t = 0.0;

    for (auto f : sv::interactive("shaped-viewer/environment-estimate"))
    {
        if (!f.imgui_wants_keyboard())
            for (auto const& e : f.input_events())
                if (e.is_key() && e.as_key().is_down && !e.as_key().is_repeat)
                {
                    if (e.as_key().character == U' ')
                        paused = !paused;
                    if (e.as_key().character == U'd')
                        denoised = !denoised;
                }
        if (!paused)
            t += cc::min(f.delta_seconds(), 0.1);

        // Around a circle, turning as it goes, high enough off the floor that its shadow and the floor under it stay apart.
        auto const angle = tg::angle_f::make_from_radians(float(t) * 0.8f);
        auto const placement = tg::affine_transform3f::make_trs(
            tg::vec3f(1.2f * tg::cos(angle), 0.9f, 1.2f * tg::sin(angle)),
            tg::quat_f::make_rotation_y(angle * 1.7f) * tg::quat_f::make_rotation_x(angle * 0.6f), tg::vec3f(1, 1, 1));

        auto grid = f.window().view().layout_grid(2, 1, {.padding = 6, .spacing = 6});
        auto const panel = [&](cc::string_view id, cc::string_view title, sv::environment_estimate estimate)
        {
            auto view = grid.add_view(id);
            view.initial_orbit({.target = tg::pos3d(0, 0.5, 0),
                                .distance = 6.5,
                                .azimuth = tg::angle_d::make_from_degree(30.0),
                                .elevation = tg::angle_d::make_from_degree(25.0)});

            auto scene = view.add_scene();
            scene.add_mesh(floor);
            scene.add_mesh(cube).transform(placement);
            scene.background(sv::background::studio());
            scene.fallback_light(cc::nullopt);
            scene.settings({.mode = sv::render_mode::hybrid,
                            .environment_estimate = estimate,
                            .reconstruct = {.denoiser = denoised ? sr::denoise_method::svgf : sr::denoise_method::none}});

            view.add_canvas().add_text(title, tg::pos2f(16, 14), {.size = 20});
        };
        panel("before", "before: sampled", sv::environment_estimate::sampled);
        panel("after", "after: exact", sv::environment_estimate::exact);
    }
}
