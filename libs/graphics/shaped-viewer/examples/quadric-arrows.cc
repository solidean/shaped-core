#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::normalize

using namespace cc::primitive_defines;

// Arrows, which are two quadrics: a capped cylinder for the shaft and a cone for the head.
//
// An arrow spans EXACTLY the segment it is given — the head is taken out of that length rather than added past its end —
// so an arrow drawn between two points measures the distance between them and nothing else.
// That is what makes the three sizing overloads mean different things rather than being three ways to say one thing:
//
//     add_arrow(s)                the proportions come from the arrow's own length (shaft 2% of it)
//     add_arrow(s, r)             the shaft radius is r and the head scales to it, so only LENGTH varies
//     add_arrow(s, style)         all three lengths given
//
// What the scene shows, back to front:
//
//     the frame          three arrows from the origin, one per axis, sized to their own length
//     the fan            one direction at eight lengths, PROPORTIONAL — every arrow a scaled copy of the last
//     the field          the same eight lengths at a FIXED shaft radius — the reading a vector field wants,
//                        since there length is the only thing an arrow's size is allowed to encode
//
// The fan and the field are the same eight segments, and the difference between the two rows is the overload alone.
//
// Colour is `per_triangle`: one value per primitive, indexed by PrimitiveIndex() — so an arrow takes two entries, and
// giving the head a lighter one is what makes the tip read at a distance.
//
// Controls
//   right-drag   look           W/A/S/D   move along the view         E / Q   rise and fall
//   shift        faster         ctrl      slower                      wheel   retune the base speed
//
// Run it:
//   uv run dev.py example shaped-viewer/quadric-arrows
//   uv run dev.py example shaped-viewer/quadric-arrows --capture

namespace
{
/// How long the i-th arrow of a row is: eight of them, growing from a stub to the row's full width.
float length_of(int i)
{
    return 0.6f + 0.42f * float(i);
}

/// Where the i-th arrow of a row stands.
/// The two rows are offset by half a column against each other, so neither hides the other from the camera.
float column_of(int i, float shift)
{
    return (float(i) - 3.5f) * 1.3f + shift;
}
} // namespace

EXAMPLE("shaped-viewer/quadric-arrows")
{
    auto arrows = sv::quadric_set();
    arrows.name = "arrows";

    auto colours = cc::vector<tg::vec3f>();

    // One colour per primitive, and an arrow is two of them — shaft then head, in the order `add_arrow` appends.
    // The head gets the lighter shade, which is what a `per_triangle` attribute is for: the two halves of one arrow are
    // one batch, one BLAS and one material, and differ only in a number indexed by PrimitiveIndex().
    auto const shade = [&](tg::vec3f const& c)
    {
        while (colours.size() < arrows.primitive_count())
            colours.push_back(colours.size() + 1 == arrows.primitive_count() ? c * 1.45f : c);
    };

    // The frame: three arrows from the origin, each sized to its own length, which is what an axis gizmo wants.
    // It stands clear of the two rows rather than among them, because it is the reference they are read against.
    auto const frame_origin = tg::pos3f(-6.4f, 0.02f, 0.4f);
    arrows.add_arrow(tg::segment3f(frame_origin, frame_origin + tg::vec3f(2.4f, 0, 0)));
    shade(tg::vec3f(0.62f, 0.15f, 0.14f));
    arrows.add_arrow(tg::segment3f(frame_origin, frame_origin + tg::vec3f(0, 2.4f, 0)));
    shade(tg::vec3f(0.17f, 0.52f, 0.20f));
    arrows.add_arrow(tg::segment3f(frame_origin, frame_origin + tg::vec3f(0, 0, 2.4f)));
    shade(tg::vec3f(0.15f, 0.30f, 0.66f));

    // The fan: eight lengths, each arrow proportional to itself.
    // Every one is a scaled copy of its neighbour, head included — which reads as one arrow at eight sizes rather than as
    // eight arrows, and is exactly why this is the wrong overload for a vector field.
    for (auto i = 0; i < 8; ++i)
    {
        auto const base = tg::pos3f(column_of(i, 0.65f), 0.35f, -2.2f);
        arrows.add_arrow(tg::segment3f(base, base + tg::vec3f(0, length_of(i), 0)));
        shade(tg::vec3f(0.80f, 0.42f, 0.16f));
    }

    // The field: the same eight segments at ONE shaft radius, so the head is the same size on every arrow and length is
    // the only thing that differs between them.
    // This is the row to compare a value across; the one behind it is not.
    for (auto i = 0; i < 8; ++i)
    {
        auto const base = tg::pos3f(column_of(i, 0.0f), 0.35f, -6.4f);
        arrows.add_arrow(tg::segment3f(base, base + tg::vec3f(0, length_of(i), 0)), 0.035f);
        shade(tg::vec3f(0.24f, 0.55f, 0.62f));
    }

    arrows.attributes.push_back(
        sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, cc::move(colours)));

    // The floor, so the arrows sit on something and cast shadows onto it.
    auto const floor
        = sv::mesh{.name = "floor",
                   .geometry = sv::triangle_geometry::create_from_positions(
                       cc::vector<tg::pos3f>{tg::pos3f(-9, 0, -9), tg::pos3f(9, 0, -9), tg::pos3f(9, 0, 5),
                                             tg::pos3f(-9, 0, -9), tg::pos3f(9, 0, 5), tg::pos3f(-9, 0, 5)}),
                   .attributes = {sv::mesh_attribute::create_value("base_color", tg::vec3f(0.22f, 0.23f, 0.25f))}};

    for (auto f : sv::interactive("shaped-viewer/quadric-arrows"))
    {
        auto view = f.window().view();

        // Yawed well off the z axis on purpose: at a near-zero yaw the frame's own +z arrow points straight at the
        // camera and reads as a stub, so a gizmo drawn to say "these are the three axes" says something else.
        view.initial_fps({.position = tg::pos3d(-5.4, 6.5, -12.8),
                          .yaw = tg::angle_d::make_from_degree(17.0),
                          .pitch = tg::angle_d::make_from_degree(-21.0)});

        view.camera_style(sv::camera_style::fly);

        auto scene = view.add_scene();
        scene.add_mesh(floor);
        scene.add_quadrics(arrows);

        // One more arrow, drawn straight into the frame's own batch rather than into a set anybody holds.
        // `scene_ref::add_arrow` is the immediate form: it appends to the batch the frame keeps per material, which is
        // what makes drawing one arrow one line instead of a set a caller has to build and keep.
        scene.add_arrow(tg::segment3f(tg::pos3f(-2.6f, 0.02f, 2.6f), tg::pos3f(4.4f, 1.6f, 2.6f)), 0.05f);

        scene.add_light({.center = tg::pos3f(-2.0f, 8.5f, -1.0f),
                         .half_extent_u = tg::vec3f(3.0f, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, 3.0f),
                         .emission = tg::vec3f(9.0f, 9.0f, 9.0f)});

        scene.background(sv::background::gradient(tg::vec3f(0.38f, 0.44f, 0.58f), tg::vec3f(0.10f, 0.11f, 0.14f)));
    }
}
