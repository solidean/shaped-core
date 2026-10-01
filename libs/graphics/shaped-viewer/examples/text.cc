#include <clean-core/common/log.hh>
#include <nexus/test.hh>
#include <shaped-rendering/slug_font.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Text over a viewer: a caption in pixel space, and labels lying on the traced cube's faces, drawn by sr::slug_routine.
//
// The viewer has no canvas yet, so this draws through `frame::draw_overlay`, the stopgap that hands a callback the
// frame's command list once every view is composited.
// The composited image carries no depth, so a label on a face that turns away still shows through the cube; depth-tested
// labels wait for the trace to write a primary-hit depth.
//
// The font is one the operating system ships: shaped-core vendors none yet.
//
// Run it:
//   uv run dev.py example shaped-viewer/text

namespace
{
/// hello-cube's cube: 12 triangles of side `size`, wound counter-clockwise from outside.
cc::vector<tg::pos3f> cube_triangles(float size)
{
    auto const h = size * 0.5f;
    auto const corner = [h](int i) { return tg::pos3f((i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h); };
    int const quads[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
    auto out = cc::vector<tg::pos3f>();
    for (auto const& q : quads)
        for (auto const k : {0, 1, 2, 0, 2, 3})
            out.push_back(corner(q[k]));
    return out;
}

cc::vector<tg::vec3f> face_colors()
{
    tg::vec3f const per_face[6]
        = {tg::vec3f(0.85f, 0.25f, 0.20f), tg::vec3f(0.20f, 0.55f, 0.85f), tg::vec3f(0.30f, 0.75f, 0.35f),
           tg::vec3f(0.90f, 0.70f, 0.20f), tg::vec3f(0.70f, 0.35f, 0.80f), tg::vec3f(0.85f, 0.85f, 0.85f)};
    auto out = cc::vector<tg::vec3f>();
    for (auto const& c : per_face)
    {
        out.push_back(c);
        out.push_back(c);
    }
    return out;
}

/// A face's label plane in world space: x along `right`, y along `up`, lifted a hair off the face toward its normal.
[[nodiscard]] tg::mat4f plane(tg::vec3f right, tg::vec3f up, tg::vec3f normal, f32 half_size)
{
    auto m = tg::mat4f::identity;
    for (auto i = 0; i < 3; ++i)
    {
        m[0, i] = right[i];
        m[1, i] = up[i];
        m[2, i] = normal[i];
        m[3, i] = normal[i] * (half_size + 0.002f);
    }
    return m;
}

/// Pixel space, y down, as clip space.
[[nodiscard]] tg::mat4f pixels_to_clip(tg::vec2i size)
{
    auto m = tg::mat4f::identity;
    m[0, 0] = 2.0f / f32(size[0]);
    m[3, 0] = -1.0f;
    m[1, 1] = -2.0f / f32(size[1]);
    m[3, 1] = 1.0f;
    m[2, 2] = 0.0f;
    return m;
}
} // namespace

EXAMPLE("shaped-viewer/text")
{
    auto loaded = sr::slug_font::load_system_ui_font();
    if (loaded.has_error())
    {
        CC_LOG_ERROR("no font to draw with: {}", loaded.error().to_string());
        return;
    }
    auto font = cc::move(loaded).value();

    auto const cube = sv::mesh{
        .name = "cube",
        .geometry = sv::triangle_geometry::create_from_positions(cube_triangles(2.0f)),
        .attributes = {sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, face_colors())}};

    // The face labels never change, so they are laid out once, each centred on its face.
    struct label
    {
        char const* text;
        tg::mat4f frame;
    };
    label const labels[] = {
        {"front", plane(tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, -1), 1.0f)},
        {"side", plane(tg::vec3f(0, 0, 1), tg::vec3f(0, 1, 0), tg::vec3f(1, 0, 0), 1.0f)},
        {"top", plane(tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1), tg::vec3f(0, 1, 0), 1.0f)},
    };
    auto label_instances = cc::vector<sr::slug_instance>();
    auto ranges = cc::vector<tg::vec2i>();
    for (auto const& l : labels)
    {
        auto const em = 0.45f;
        auto const first = i32(label_instances.size());
        font.append_line(label_instances, l.text, tg::pos2f(-font.line_width(l.text, em) * 0.5f, -em * 0.35f), em,
                         tg::vec4f(0.06f, 0.06f, 0.09f, 1));
        ranges.push_back(tg::vec2i(first, i32(label_instances.size()) - first));
    }

    // The overlay draws instances and glyphs uploaded on the same list, so vulkan splits its scope at the first draw.
    // Nothing states a scope's accesses before it opens yet; libs/graphics/shaped-graphics/docs/TODO.md, "Barriers + access tracking".
    nx::allow_warnings("was closed and reopened around a barrier", "sg");

    for (auto f : sv::interactive("shaped-viewer/text"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0),
                            .distance = 6.0,
                            .azimuth = tg::angle_d::make_from_degree(35.0),
                            .elevation = tg::angle_d::make_from_degree(25.0)});

        auto scene = view.add_scene();
        scene.add_mesh(cube);
        scene.add_rect_light("key", tg::pos3f(0, 3, 0), tg::vec3f(0.9f, 0, 0), tg::vec3f(0, 0, 0.9f)).nits(14.0f);
        scene.background(sv::background::gradient(tg::vec3f(0.70f, 0.96f, 1.44f), tg::vec3f(0.21f, 0.28f, 0.37f)));

        f.draw_overlay(
            [&](sv::overlay_context const& o)
            {
                auto caption = cc::vector<sr::slug_instance>();
                font.append_line(caption, "shaped-viewer, with text", tg::pos2f(24, 48), 32.0f, tg::vec4f(1, 1, 1, 1),
                                 tg::vec2f(1, 0), tg::vec2f(0, -1));
                font.append_line(caption, "labels lie on the traced cube; the caption is in pixels", tg::pos2f(26, 80),
                                 17.0f, tg::vec4f(1, 1, 1, 1), tg::vec2f(1, 0), tg::vec2f(0, -1));

                // Every upload before the scope: the glyphs the atlas gained, and both instance arrays.
                auto const placed = sr::slug_routine::prepare(o.cmd, font.atlas(), label_instances);
                auto const captioned = sr::slug_routine::prepare(o.cmd, font.atlas(), caption);

                auto pass = o.cmd.raster.render_to({.color_targets = {o.target.preserved()}});
                for (auto i = isize(0); i < ranges.size(); ++i)
                    (void)sr::slug_routine::execute(pass, font.atlas(), placed.instances, ranges[i][0], ranges[i][1],
                                                    {.object_to_clip = o.world_to_clip * labels[i].frame});
                (void)sr::slug_routine::execute(pass, font.atlas(), captioned,
                                                {.object_to_clip = pixels_to_clip(o.size)});
            });
    }
}
