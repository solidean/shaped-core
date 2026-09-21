#include "viewer_test_env.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/view/camera.hh>
#include <sv_test_shaders.hh>

using namespace cc::primitive_defines;

// The motion guide the raygen writes, measured against the camera it claims to invert.
//
// A temporal denoiser fed wrong motion vectors does not fail — it smears, and an image that is merely soft reads as a
// denoiser doing its job.
// So nothing downstream catches this, and reading the guide out of a rendered frame cannot tell a wrong vector from a
// wrong trace.
// `shaders/camera_probe.hlsl` therefore calls the raygen's own `camera_project` and `camera_ray_offset`, and every
// number below comes back from the GPU rather than from a second implementation here.

namespace
{
/// Mirrors `sv::camera_probe_case` in shaders/camera_probe.hlsl lane-for-lane.
struct camera_probe_case
{
    sv::camera_gpu cam;
    sv::camera_gpu prev;

    tg::vec2f pixel;
    tg::vec2f dim;

    tg::vec3f world;
    u32 at_infinity = 0;
};

/// What one case measured: the motion vector the raygen would write, and `pixel` round-tripped through the camera.
struct probe_result
{
    tg::vec2f motion = tg::vec2f(0, 0);
    tg::vec2f round_trip = tg::vec2f(0, 0);
};

/// The camera every case below starts from: at the origin's -z side, looking down +z, square image.
[[nodiscard]] sv::camera base_camera()
{
    auto cam = sv::camera::looking_at(tg::pos3d(0, 0, -5), tg::pos3d::zero);
    cam.projection.aspect_ratio = 1.0;
    return cam;
}

/// Dispatches `cases` and reads one result back per case.
///
/// Built inline rather than behind a routine, exactly as the BSDF probe is: nothing a viewer runs dispatches this, so a
/// routine would put test-only machinery in the library.
cc::shared_async<cc::vector<probe_result>> run_probe(sg::context& ctx, cc::span<camera_probe_case const> cases)
{
    auto const shader = sv_test::shaders::camera_probe.compute.CameraProbe->acquire(ctx);
    co_await cc::async_settled(shader);
    if (shader->has_error())
        FAIL(cc::format("the camera probe shader did not compile:\n{}", shader->try_error()->underlying().to_string()));

    auto const* const compiled = shader->try_value();
    REQUIRE(compiled != nullptr); // without it every check below is vacuous

    auto const group_layout = ctx.cached.acquire_binding_group_layout<sv_test::shaders::camera_probe_bindings>();
    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}});
    auto pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = pipeline_layout});
    auto const built = co_await pipeline;
    REQUIRE(built != nullptr);

    auto cmd = ctx.create_command_list();

    auto const case_buffer = ctx.transient.create_buffer<camera_probe_case>(
        cases.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(case_buffer, cases);

    auto const result_buffer = ctx.transient.create_buffer<tg::vec4f>(
        cases.size(), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto const group = ctx.transient.create_binding_group(
        group_layout, sv_test::shaders::camera_probe_bindings{.Cases = case_buffer.as_readonly_buffer(),
                                                              .Results = result_buffer.as_readwrite_buffer()});

    cmd->compute.bind_pipeline(*built);
    cmd->compute.bind<sv_test::shaders::camera_probe_bindings>(*group);
    cmd->compute.dispatch_threads(cases.size());

    auto readback = cmd->download.data_from_buffer(result_buffer);

    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const items = co_await readback.data();
    REQUIRE(items.size() == cases.size());

    auto out = cc::vector<probe_result>();
    out.reserve(cases.size());
    for (auto const& i : items)
        out.push_back({.motion = tg::vec2f(i[0], i[1]), .round_trip = tg::vec2f(i[2], i[3])});
    co_return out;
}
} // namespace

// Projection is the inverse of the primary ray, which is the whole assumption a reprojection rests on.
//
// The corners are in because that is where the two disagree first: a sign or an aspect folded in on one side alone
// stays invisible at the centre, where everything is zero.
ASYNC_INVOCABLE_TEST("sv - a camera projects its own primary ray back to the pixel it came from",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the probe shader");

    auto const dim = tg::vec2f(256, 256);
    auto const cam = sv::camera_gpu::from(base_camera());

    auto const pixels = cc::vector<tg::vec2f>{
        tg::vec2f(128, 128), // centre
        tg::vec2f(0.5f, 0.5f),   tg::vec2f(255.5f, 0.5f),
        tg::vec2f(0.5f, 255.5f), tg::vec2f(255.5f, 255.5f),  // corners
        tg::vec2f(40, 200),      tg::vec2f(201.25f, 17.75f), // off-axis
    };

    auto cases = cc::vector<camera_probe_case>();
    for (auto const& p : pixels)
        cases.push_back({.cam = cam, .prev = cam, .pixel = p, .dim = dim, .world = tg::vec3f(0, 0, 0)});

    auto const results = co_await run_probe(ctx, cases);
    for (auto i = isize(0); i < pixels.size(); ++i)
    {
        auto const d = results[i].round_trip - pixels[i];
        CHECK(tg::abs(d[0]) < 0.01f).context(cc::format("pixel {} came back as {}", pixels[i][0], results[i].round_trip[0]));
        CHECK(tg::abs(d[1]) < 0.01f).context(cc::format("pixel {} came back as {}", pixels[i][1], results[i].round_trip[1]));
    }
}

// What the guide must say about a camera that did not move, about one that turned, and about one that stepped sideways.
//
// The sky case is the one worth having: an escaped ray reprojects as a point at INFINITY, so translating the camera
// must not move it at all while rotating must.
// Reproject the sky as a point at the camera's own position instead — the obvious mistake — and a pan smears the
// background against the geometry, which is the artifact nobody attributes to a motion vector.
ASYNC_INVOCABLE_TEST("sv - motion vectors follow the camera, and the sky ignores translation",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the probe shader");

    auto const dim = tg::vec2f(256, 256);
    auto const centre = tg::vec2f(128, 128);

    // The point the centre pixel's ray hits: straight ahead of the base camera, on its axis.
    auto const world = tg::vec3f(0, 0, 0);

    auto const still = base_camera();

    // A step to the camera's own right, small enough that the point stays well inside the image.
    auto stepped = still;
    stepped.position = tg::pos3d(0.5, 0, -5);

    // A turn to the left of the same order, so the two cases are comparable in magnitude.
    auto turned = still;
    turned.look_at(tg::pos3d(-0.5, 0, 0));

    enum : isize
    {
        c_unchanged = 0,
        c_translated,
        c_translated_sky,
        c_turned_sky,
        c_count,
    };

    auto cases = cc::vector<camera_probe_case>();
    cases.resize_to_defaulted(c_count);
    for (auto& c : cases)
    {
        c.cam = sv::camera_gpu::from(still);
        c.pixel = centre;
        c.dim = dim;
        c.world = world;
    }
    cases[c_unchanged].prev = sv::camera_gpu::from(still);
    cases[c_translated].prev = sv::camera_gpu::from(stepped);
    cases[c_translated_sky].prev = sv::camera_gpu::from(stepped);
    cases[c_translated_sky].at_infinity = 1;
    cases[c_turned_sky].prev = sv::camera_gpu::from(turned);
    cases[c_turned_sky].at_infinity = 1;

    auto const r = co_await run_probe(ctx, cases);

    // A camera that did not move reprojects every pixel onto itself, so the guide is exactly zero.
    // Exactly, rather than nearly: the denoiser reads a non-zero vector on a still frame as motion, and a still frame
    // is when its history is worth the most.
    CHECK(tg::abs(r[c_unchanged].motion[0]) < 0.01f)
        .context(cc::format("a still camera moved by {}", r[c_unchanged].motion[0]));
    CHECK(tg::abs(r[c_unchanged].motion[1]) < 0.01f)
        .context(cc::format("a still camera moved by {}", r[c_unchanged].motion[1]));

    // The previous camera stood to the RIGHT of this one, so the point sat further left on its image, and this frame's
    // pixel minus that one is positive.
    // The sign is the half of a motion vector that is easy to get backwards and impossible to see.
    CHECK(r[c_translated].motion[0] > 1.0f).context(cc::format("a sideways step gave {}", r[c_translated].motion[0]));
    CHECK(tg::abs(r[c_translated].motion[1]) < 0.01f); // the step was horizontal, so nothing moved vertically

    // Parallax is the whole difference between the two: the sky is infinitely far away, so a step does not move it.
    CHECK(tg::abs(r[c_translated_sky].motion[0]) < 0.01f)
        .context(cc::format("the sky moved by {} under a pure translation", r[c_translated_sky].motion[0]));

    // ...and a turn does, which is what makes the case above a measurement rather than a stuck zero.
    CHECK(tg::abs(r[c_turned_sky].motion[0]) > 1.0f)
        .context(cc::format("the sky moved by {} under a turn", r[c_turned_sky].motion[0]));
}

// A hit that was BEHIND the previous camera has no pixel to come from, and the guide has to say so rather than
// reporting a plausible one.
//
// `camera_project` answers far off the image, which a reprojection reads as "not visible last frame" — the same answer
// it gives for a point that was simply off-screen, and the only one that does not invent history.
ASYNC_INVOCABLE_TEST("sv - a point behind the previous camera reprojects off the image",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the probe shader");

    auto const dim = tg::vec2f(256, 256);

    // The previous camera sat past the point, looking further along +z, so the point is behind it.
    auto behind = base_camera();
    behind.position = tg::pos3d(0, 0, 5);

    auto const cases = cc::vector<camera_probe_case>{{.cam = sv::camera_gpu::from(base_camera()),
                                                      .prev = sv::camera_gpu::from(behind),
                                                      .pixel = tg::vec2f(128, 128),
                                                      .dim = dim,
                                                      .world = tg::vec3f(0, 0, 0)}};

    auto const r = co_await run_probe(ctx, cases);

    // Far off, not merely outside: a denoiser clamps its history lookup, so "just off the edge" would sample the border
    // rather than reject the pixel.
    CHECK(r[0].motion[0] > dim[0]).context(cc::format("a point behind the camera reprojected to {}", r[0].motion[0]));
}

// The matrices `sv::matrices_of` hands a denoiser, against the frustum the raygen actually traces.
//
// sv rasterizes nothing, so these two matrices have no other use and nothing else would notice them being wrong.
// A denoiser reprojecting through a transposed or mis-scaled projection ghosts rather than fails, which is the same
// failure mode the probe above exists for.
//
// Checked as properties of the frustum rather than by recomputing the formula: `forward`, `right_scaled` and
// `up_scaled` ARE the frustum's axes — a point one `right_scaled` off the axis sits exactly on its right edge — so a
// folded aspect ratio, a swapped axis or a flipped sign each move one of these numbers and none of them survives.
TEST("sv - a camera's matrices describe the frustum its rays sweep")
{
    // Off every axis, with a rolled-up vector, and not square.
    // Each of those hides a different mistake: an axis-aligned camera's rotation is symmetric, so a transposed
    // world-to-view passes; a level one has no roll to lose; and a 1:1 image cancels a misplaced aspect ratio.
    auto camera = sv::camera::looking_at(tg::pos3d(3.0, 2.0, -4.0), tg::pos3d(0.5, -0.3, 1.0), tg::vec3d(0.2, 0.9, 0.1));
    camera.projection.aspect_ratio = 16.0 / 9.0;
    auto const cam = sv::camera_gpu::from(camera);

    auto const near_plane = 0.25f;
    auto const m = sv::matrices_of(cam, near_plane);

    /// A world point through both matrices, as (ndc_x, ndc_y, view_depth).
    auto const project = [&](tg::vec3f world)
    {
        auto const view = m.world_to_view * tg::vec4f(world[0], world[1], world[2], 1.0f);
        auto const clip = m.view_to_clip * view;
        return tg::vec3f(clip[0] / clip[3], clip[1] / clip[3], clip[3]);
    };

    auto const eye = cam.position;
    auto const forward = cam.forward / cam.forward.length();

    // The camera sits at the view origin, and one unit along its forward axis is one unit of view depth.
    auto const at_eye = m.world_to_view * tg::vec4f(eye[0], eye[1], eye[2], 1.0f);
    for (auto axis = 0; axis < 3; ++axis)
        CHECK(tg::abs(at_eye[axis]) < 1e-4f).context(cc::format("the eye is not the view origin on axis {}", axis));

    // The forward axis is the image centre, whatever the aspect ratio.
    auto const centre = project(eye + forward);
    CHECK(tg::abs(centre[0]) < 1e-4f).context(cc::format("centre ndc x is {}", centre[0]));
    CHECK(tg::abs(centre[1]) < 1e-4f).context(cc::format("centre ndc y is {}", centre[1]));
    CHECK(tg::abs(centre[2] - 1.0f) < 1e-4f).context(cc::format("view depth at one unit ahead is {}", centre[2]));

    // One `right_scaled` off the axis is the right edge, and one `up_scaled` the top — which is what those vectors
    // mean to `camera_ray_offset`.
    // y is up here where the raygen's pixel space is y down, so the top edge is +1 rather than -1.
    auto const right_edge = project(eye + forward + cam.right_scaled);
    CHECK(tg::abs(right_edge[0] - 1.0f) < 1e-4f).context(cc::format("right edge ndc x is {}", right_edge[0]));
    CHECK(tg::abs(right_edge[1]) < 1e-4f).context(cc::format("right edge ndc y is {}", right_edge[1]));

    auto const top_edge = project(eye + forward + cam.up_scaled);
    CHECK(tg::abs(top_edge[0]) < 1e-4f).context(cc::format("top edge ndc x is {}", top_edge[0]));
    CHECK(tg::abs(top_edge[1] - 1.0f) < 1e-4f).context(cc::format("top edge ndc y is {}", top_edge[1]));

    // The two edges sit at different distances from the axis, which is the aspect ratio riding on exactly one of them.
    // Equal lengths here would mean it was folded into both, or into neither.
    CHECK(cam.right_scaled.length() > cam.up_scaled.length() * 1.5f);

    // Depth is the view depth rather than a distance, so a point off the axis is no further away than its z.
    auto const off_axis = project(eye + forward * 3.0f + cam.right_scaled * 0.5f);
    CHECK(tg::abs(off_axis[2] - 3.0f) < 1e-3f).context(cc::format("off-axis view depth is {}", off_axis[2]));

    // Clip depth is z / w, so the near plane is where it reaches zero.
    auto const near_point = project(eye + forward * near_plane);
    auto const near_clip = m.view_to_clip
                         * (m.world_to_view
                            * tg::vec4f((eye + forward * near_plane)[0], (eye + forward * near_plane)[1],
                                        (eye + forward * near_plane)[2], 1.0f));
    CHECK(tg::abs(near_clip[2]) < 1e-4f).context(cc::format("clip z at the near plane is {}", near_clip[2]));
    CHECK(tg::abs(near_point[2] - near_plane) < 1e-4f);
}
