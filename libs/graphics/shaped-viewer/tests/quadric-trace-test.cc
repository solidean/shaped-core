#include "viewer_test_env.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/context.hh> // sv::background_work
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/resources/quadric_data.hh>
#include <shaped-viewer/resources/resource_managers.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::normalize

using namespace cc::primitive_defines;

// The first trace of an analytic quadric: a procedural BLAS, a generated intersection shader, and the shared shading tail.
//
// Everything below this was CPU values and GPU buffers nothing had ever traced.
// What this pins is that the whole chain lines up — the primitive record the CPU packs is the one the intersection shader
// decodes, the AABBs the BLAS was built from actually bound the surface, and the hit reaches `pt_shade` with a usable normal.
//
// The check is the SILHOUETTE rather than the routine's outcome, and that is the point.
// A trace that dispatches and hits nothing returns `executed` exactly as one that draws the sphere does, so asserting on the
// outcome alone would pass with a completely broken intersection shader — which is the failure this phase could most plausibly
// have shipped.
// A sphere of radius 1 at distance 4 under a 60-degree vertical fov covers a known fraction of the frame, so the count of
// covered pixels is a real check on the geometry rather than on the plumbing.

namespace
{
constexpr i32 image_size = 32;
constexpr float env_radiance = 1.0f;

float luminance_of(tg::vec4f const& px)
{
    return 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
}

/// The fraction of the frame a sphere of `radius` at `distance` covers, under a vertical field of view of `fov_deg`.
///
/// Computed rather than measured once and pasted, so the expectation moves with the camera if this test is ever retuned.
/// The sphere's silhouette is the cone tangent to it, whose half-angle is asin(radius / distance).
double covered_fraction(double radius, double distance, double fov_deg)
{
    auto const half_fov = fov_deg * 0.5 * 3.14159265358979323846 / 180.0;
    auto const half_extent = tg::tan(tg::angle_d::make_from_radians(half_fov)); // half-height at unit distance
    auto const tangent = tg::asin(radius / distance);
    auto const silhouette = tg::tan(tangent); // the silhouette's half-extent, same units

    auto const r = silhouette / half_extent;      // as a fraction of the half-image
    return 3.14159265358979323846 * r * r * 0.25; // a disc of that radius, over the full [-1,1]^2 frame
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a quadric sphere is traced through a procedural BLAS", (sg::context_handle const& ctx_h))
{
    // The same inline-readback path volumetric-furnace-test documents as fastfailing on Windows on ARM.
    // Skipped for the same reason and on the same evidence; the trace itself is not what breaks there.
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif

    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the quadric shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);

    // One sphere of radius 1 at the origin — the smallest thing that proves the chain works end to end.
    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));

    auto const batch = resources.quadrics.acquire(sv::quadric_data::of(set));
    resources.wait_for_pending_uploads();

    auto const* const record = resources.quadrics.get_ptr(batch);
    REQUIRE(record != nullptr);
    REQUIRE(record->state == sv::residency::complete);
    REQUIRE(record->blas != nullptr);

    // The neutral quadric permutation: one hard-coded gray surface, which is all a first trace needs.
    auto const& permutation = resources.shaders.acquire_quadric_fallback();

    // What makes its hit group PROCEDURAL, and the one thing a triangle permutation never carries.
    REQUIRE(permutation.intersection.is_valid());

    // Driven and checked here rather than left to the trace loop below: a shader that does not compile makes the trace
    // decline forever, which arrives as a timeout with nothing to point at instead of as DXC's own message.
    co_await cc::async_settled(permutation.shader);
    if (permutation.shader->has_error())
        FAIL(cc::format("quadric closest-hit: {}\n--- source ---\n{}",
                        permutation.shader->try_error()->underlying().to_string(), permutation.source));

    co_await cc::async_settled(permutation.intersection);
    if (permutation.intersection->has_error())
        FAIL(cc::format("quadric intersection: {}\n--- source ---\n{}",
                        permutation.intersection->try_error()->underlying().to_string(), permutation.source));

    REQUIRE(permutation.intersection->try_value() != nullptr);
    CHECK(permutation.intersection->try_value()->stage == sg::shader_stage::intersection);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = record->blas, .instance_id = 0, .hit_group_offset = 0});

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(&permutation);

    auto camera = sv::camera::looking_at(tg::pos3d(0, 0, 4), tg::pos3d::zero);
    camera.projection.aspect_ratio = 1.0;

    auto const trace = [&](sg::command_list& cmd, sg::data_future<tg::vec4f>& readback)
    {
        // The intersection shader reaches the batch's primitives through the instance's `vertices`, which for a quadric
        // batch names the primitive buffer rather than a position buffer.
        auto const primitives = u32(resources.acquire_buffer(record->primitives.raw()->as_raw_readonly()));

        auto records = cc::vector<sv::instance_gpu>();
        records.push_back({.param_buffer = primitives, // the fallback reads no parameter block, so any valid index serves
                           .param_offset = 0,
                           .vertices = primitives,
                           .indices = primitives,
                           .is_indexed = 0});

        auto const frame = ctx.transient.create_buffer_from_pod(
            cmd,
            sv::pt_frame_constants_gpu{.camera = sv::camera_gpu::from(camera), .samples_per_pixel = 8, .max_bounces = 2},
            sg::buffer_usage::constants_buffer);

        // A uniform environment, so the background is a known constant and anything darker than it was HIT.
        auto const background = ctx.transient.create_buffer_from_pod(
            cmd, sv::background_gpu::from(sv::background::uniform(tg::vec3f(env_radiance, env_radiance, env_radiance))),
            sg::buffer_usage::constants_buffer);

        auto const target = ctx.transient.create_texture_2d(
            {.format = sg::pixel_format::rgba32_float,
             .width = image_size,
             .height = image_size,
             // copy_src as well, because this test reads the image back rather than only asserting on the outcome.
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});

        auto const instance_table
            = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer);

        auto const bindless = resources.freeze();
        auto const outcome = sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                                  .background = background,
                                                                  .instances = instances,
                                                                  .output = target,
                                                                  .instance_table = instance_table,
                                                                  .hit_groups = hit_groups,
                                                                  .bindless = &bindless});

        if (outcome == sg::routine_outcome::executed)
            readback = sg::data_future<tg::vec4f>(cmd.download.bytes_from_texture(target.raw()));

        return outcome;
    };

    // The pipeline carrying a procedural hit group is built asynchronously like any other, so the first frames decline.
    auto pixels = cc::vector<tg::vec4f>();
    auto const loop_start = cc::current_time_steady_secs();
    while (pixels.empty())
    {
        // Once per frame, before the frame's first acquire: without it no routine ever becomes ready and this loop
        // would time out with nothing to point at but a warning on stderr.
        (void)ctx.routines.tick();

        auto readback = sg::data_future<tg::vec4f>();
        auto cmd = ctx.create_command_list();
        auto const outcome = trace(*cmd, readback);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();

        if (outcome != sg::routine_outcome::executed)
        {
            // Not a failure yet — the state object has not landed.
            // The deadline is what turns a hang into a message.
            REQUIRE(cc::current_time_steady_secs() - loop_start < 45.0);
            sv_test::drive_ambient_work(); // under SC_THREADS=OFF this thread is the only one that can build it
            continue;
        }

        REQUIRE(readback.is_valid());
        co_await ctx.idle_completion(); // an epoch advance drains the GPU but not the readback actor

        auto const delivered = readback.try_get_data();
        REQUIRE(delivered.has_value());
        auto const span = delivered.value().span();
        pixels = cc::vector<tg::vec4f>::create_defaulted(span.size());
        for (auto i = isize(0); i < span.size(); ++i)
            pixels[i] = span[i];
    }

    REQUIRE(pixels.size() == isize(image_size) * isize(image_size));

    auto const at = [&](i32 x, i32 y) { return pixels[y * image_size + x]; };

    // Anything materially darker than the environment was hit: the surface is gray and takes two bounces, so it cannot
    // come back as bright as the background it replaced.
    auto const is_hit = [&](tg::vec4f const& px) { return luminance_of(px) < env_radiance * 0.9f; };

    // The center of the frame is the center of the sphere.
    CHECK(is_hit(at(image_size / 2, image_size / 2)));

    // The corners are well outside a sphere that covers about a sixth of the frame, so they must be background.
    CHECK(!is_hit(at(0, 0)));
    CHECK(!is_hit(at(image_size - 1, 0)));
    CHECK(!is_hit(at(0, image_size - 1)));
    CHECK(!is_hit(at(image_size - 1, image_size - 1)));

    // The real check: how much of the frame the silhouette covers.
    // A wrong quadratic, a wrong record stride or a box that does not bound the surface all change this number, where none
    // of them would change the routine's outcome.
    auto covered = isize(0);
    for (auto const& px : pixels)
        if (is_hit(px))
            ++covered;

    auto const fraction = double(covered) / double(pixels.size());
    auto const expected = covered_fraction(1.0, 4.0, 60.0);

    // Generous bounds for a 32x32 frame, where the silhouette is about 14 pixels across and its edge is a whole pixel wide.
    CHECK(fraction > expected * 0.7);
    CHECK(fraction < expected * 1.3);

    // A hit shaded through a broken normal comes back black rather than gray, which the coverage count alone would accept.
    for (auto const& px : pixels)
        if (is_hit(px))
            CHECK(luminance_of(px) > 0.01f);

    // Everything this test set going, including what the routine started on the frame path.
    // `sv::background_work` is the context-wide answer, and that is the point: a trace kicks off compiles nothing
    // here holds a handle to, so naming individual nodes cannot cover them.
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a quadric material reading a per-primitive attribute compiles",
                     (sg::context_handle const& ctx_h))
{
    // The generated body for a quadric is the same text a mesh would get — that is the point of one shared frequency set —
    // so what a real compile checks is that the quadric PREAMBLE supplies everything that body reads.
    // quadric-material-test asserts the emitted text; only a compile says the text means anything.
    auto& ctx = *ctx_h;

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the quadric shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);

    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("color", tg::vec3f(0.5f, 0.5f, 0.5f)));
    auto const type
        = sv::material_type::create("sv_test_per_primitive", cc::move(signature), "    surface.base_color = color;");
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    // One value per primitive, which is the finest a quadric batch can serve.
    auto const colors = cc::array<tg::vec3f>::create_filled(2, tg::vec3f(1, 0, 0));
    auto const attribute = sv::mesh_attribute::create("color", sv::attribute_frequency::per_triangle, colors);

    auto set = sv::resident_quadric_set{.name = "edges", .geometry = sv::quadric_set_id(0), .primitive_count = 2};
    set.attributes.push_back(sv::mesh_attribute_binding::of(attribute, sv::attribute_id(0)));

    auto const resolved = sv::resolve_material(type, material, set);
    REQUIRE(resolved.attributes.size() == 1);
    REQUIRE(resolved.attributes[0].attribute != nullptr);
    CHECK(resolved.attributes[0].attribute->frequency == sv::attribute_frequency::per_triangle);

    auto const& permutation = resources.shaders.acquire_quadric(resolved);

    co_await cc::async_settled(permutation.shader);
    if (permutation.shader->has_error())
        FAIL(cc::format("quadric closest-hit: {}\n--- source ---\n{}",
                        permutation.shader->try_error()->underlying().to_string(), permutation.source));

    co_await cc::async_settled(permutation.intersection);
    if (permutation.intersection->has_error())
        FAIL(cc::format("quadric intersection: {}\n--- source ---\n{}",
                        permutation.intersection->try_error()->underlying().to_string(), permutation.source));

    CHECK(permutation.shader->try_value()->stage == sg::shader_stage::closest_hit);
    CHECK(permutation.intersection->try_value()->stage == sg::shader_stage::intersection);

    // The same material against a MESH is a different permutation, since the load code differs — which is the two-spellings
    // property the whole fork rests on.
    auto mesh = sv::resident_mesh{.name = "tri", .geometry = sv::mesh_id(0), .triangle_count = 1, .vertex_count = 3};
    auto const on_mesh = sv::resolve_material(type, material, mesh);
    CHECK(on_mesh.permutation_key != resolved.permutation_key);

    // Everything this test set going, including what the routine started on the frame path.
    // `sv::background_work` is the context-wide answer, and that is the point: a trace kicks off compiles nothing
    // here holds a handle to, so naming individual nodes cannot cover them.
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a quadric batch is placed through the resource manager", (sg::context_handle const& ctx_h))
{
    // The authoring path end to end: a set a caller holds, placed the way `scene_ref::add_quadrics` places one.
    // What it pins is that ONE resolution produces the three fields a scene item carries, and that re-placing an unchanged
    // set costs lookups rather than uploads — the property the whole per-frame authoring model rests on.
    auto& ctx = *ctx_h;

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the quadric shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);

    auto set = sv::quadric_set();
    set.name = "edges";
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 0.2f));
    set.add_line(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0)), 0.05f);

    // One value per primitive — the set holds two — which is what makes this a per_triangle material rather than a flat one.
    auto const colors = cc::array<tg::vec3f>{tg::vec3f(1, 0, 0), tg::vec3f(0, 0, 1)};
    set.attributes.push_back(sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, colors));

    auto const item = resources.acquire_scene_item(set);

    CHECK(item.kind == sv::scene_item_kind::quadric_set);
    CHECK(item.quadrics != sv::quadric_set_id::invalid);
    CHECK(item.mesh == sv::mesh_id::invalid); // the arm a quadric item does NOT use
    CHECK(resources.contains_instance(item.instance));

    // The permutation the resolution yielded is the quadric spelling, so it carries an intersection shader.
    auto const* const permutation = resources.shaders.find(item.shader_key);
    REQUIRE(permutation != nullptr);
    CHECK(permutation->intersection.is_valid());

    co_await cc::async_settled(permutation->shader);
    if (permutation->shader->has_error())
        FAIL(cc::format("quadric closest-hit: {}\n--- source ---\n{}",
                        permutation->shader->try_error()->underlying().to_string(), permutation->source));
    co_await cc::async_settled(permutation->intersection);
    REQUIRE(permutation->intersection->has_value());

    // Placing the same set again is the same everything: the slot short-circuits, and the content hashes behind it agree.
    auto const again = resources.acquire_scene_item(set);
    CHECK(again.quadrics == item.quadrics);
    CHECK(again.instance == item.instance);
    CHECK(again.shader_key == item.shader_key);

    resources.wait_for_pending_uploads();
    CHECK(set.is_ready() == false); // `is_ready` is a snapshot of the last PLACEMENT, and that one predates the upload

    auto const third = resources.acquire_scene_item(set);
    CHECK(third.quadrics == item.quadrics);
    CHECK(set.is_ready()); // re-placed after the upload landed, so the slot now says so

    // And a batch whose geometry differs is a different resource, or a placement would draw the wrong thing.
    auto other = sv::quadric_set();
    other.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 0.3f));
    auto const other_item = resources.acquire_scene_item(other);
    CHECK(other_item.quadrics != item.quadrics);

    // A batch with no attributes resolves differently, so it is a second permutation and a second pair of compiles.
    // Driven here because a node this test started is async work still holding its context when it ends, which nexus
    // reports as a failure of the test itself.
    auto const* const other_permutation = resources.shaders.find(other_item.shader_key);
    REQUIRE(other_permutation != nullptr);
    CHECK(other_permutation->key != permutation->key);
    co_await cc::async_settled(other_permutation->shader);
    co_await cc::async_settled(other_permutation->intersection);

    // `other` was acquired AFTER the drain above, and acquiring a batch queues two stream uploads — its primitive
    // buffer and its box buffer.
    // Those are the two async items this test used to leave running: with a warm cache and a thread pool they happen
    // to land during the compiles awaited just above, and on the single-threaded preset they do not.
    resources.wait_for_pending_uploads();

    // Everything else this test set going, including what the routine started on the frame path.
    // `sv::background_work` is the context-wide answer, and that is the point: a trace kicks off compiles nothing
    // here holds a handle to, so naming individual nodes cannot cover them.
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - the traced silhouette agrees with the CPU reference", (sg::context_handle const& ctx_h))
{
    // `sv::intersect` and `intersect_quadric` are written to mirror each other, and until now that was asserted by comment.
    // This runs BOTH against the same rays and compares what they hit.
    //
    // The comparison is the silhouette rather than the shading: what the two routines share is which points are on the
    // solid, and a color would drag the whole BSDF into a test about geometry.
    //
    // The shapes are the ones with the least other coverage — a capped cylinder, a cone frustum and a hyperboloid — since
    // an agreement test is worth most exactly where neither side has been checked against anything else.
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif

    // Bigger than the other tests on purpose.
    // The comparison can only differ where the silhouette crosses a pixel, so the test's strength is the ratio of interior
    // to perimeter — and at 32x32 a shape this size is nearly all perimeter, which would pass with a badly wrong shader.
    constexpr i32 agreement_size = 128;

    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the quadric shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);

    auto set = sv::quadric_set();

    // A capped cylinder: the clipper's own surface is drawn, so the caps are part of what has to agree.
    set.add_line(tg::segment3f(tg::pos3f(-1.1f, -0.6f, 0), tg::pos3f(-1.1f, 0.6f, 0)),
                 {.radius = 0.45f, .ends = sv::line_ends::flat});

    // A cone frustum, built by hand as the gallery example builds its own.
    {
        constexpr float slope = 0.45f;
        auto cone = sv::quadric_primitive();
        cone.origin = tg::pos3f(0, 1.4f, 0);
        cone.surface = {.diag = tg::vec3f(1.0f, -slope * slope, 1.0f)};
        cone.clip = sv::quadric3::slab(tg::vec3f(0, 1, 0), -1.4f, 0.6f); // a slice below the apex
        cone.flags = sv::quadric_primitive::flag_emit_clip_surface;
        cone.bounds = tg::aabb3f(tg::pos3f(-0.9f, -0.6f, -0.9f), tg::pos3f(0.9f, 0.6f, 0.9f));
        set.add(cone);
    }

    // A hyperboloid of one sheet, clipped to a slab.
    {
        auto hyp = sv::quadric_primitive();
        hyp.origin = tg::pos3f(1.3f, 0, 0);
        hyp.surface = {.diag = tg::vec3f(1.0f, -1.0f, 1.0f), .constant = -0.16f};
        hyp.clip = sv::quadric3::slab_about_origin(tg::vec3f(0, 1, 0), 0.6f);
        auto const reach = tg::sqrt(0.16f + 0.36f);
        hyp.bounds = tg::aabb3f(tg::pos3f(1.3f - reach, -0.6f, -reach), tg::pos3f(1.3f + reach, 0.6f, reach));
        set.add(hyp);
    }

    // The rest of the family, in a row behind the three above, so ONE trace covers every shape the gallery draws.
    //
    // The three in front are the ones with the least other coverage; these four are the ones the CPU tests already pin
    // individually, and the point of having them here is different — it is that the two implementations agree about them,
    // not that either is right on its own.
    constexpr float back_z = -2.2f;

    // A sphere, and an ellipsoid: the same surface family under a diagonal that is not the identity.
    set.add_sphere(tg::sphere3f(tg::pos3f(-1.9f, 0.1f, back_z), 0.5f));
    {
        auto ell = sv::quadric_primitive();
        ell.origin = tg::pos3f(-0.7f, 0.1f, back_z);
        auto const r = tg::vec3f(0.62f, 0.34f, 0.45f);
        ell.surface
            = {.diag = tg::vec3f(1.0f / (r[0] * r[0]), 1.0f / (r[1] * r[1]), 1.0f / (r[2] * r[2])), .constant = -1.0f};
        ell.bounds = tg::aabb3f(ell.origin - r, ell.origin + r);
        set.add(ell);
    }

    // An OPEN tube, which is the far-root case: seen near end-on its near root is outside the slab, and the visible
    // surface is the inside of the opposite wall.
    // The two implementations have to agree about that too.
    set.add_line(tg::segment3f(tg::pos3f(0.5f, -0.35f, back_z), tg::pos3f(0.5f, 0.55f, back_z)),
                 {.radius = 0.34f, .ends = sv::line_ends::open});

    // A hemisphere: a sphere clipped by a plane pair, with the clipper's own surface drawn as the floor.
    {
        auto hemi = sv::quadric_primitive();
        hemi.origin = tg::pos3f(1.7f, -0.3f, back_z);
        hemi.surface = sv::quadric3::sphere_about_origin(0.55f);
        hemi.clip = sv::quadric3::slab(tg::vec3f(0, 1, 0), 0.275f, 0.275f); // p.y in [0, 0.55] — the upper half
        hemi.flags = sv::quadric_primitive::flag_emit_clip_surface;
        hemi.bounds = tg::aabb3f(tg::pos3f(1.15f, -0.3f, back_z - 0.55f), tg::pos3f(2.25f, 0.25f, back_z + 0.55f));
        set.add(hemi);
    }

    auto const batch = resources.quadrics.acquire(sv::quadric_data::of(set));
    resources.wait_for_pending_uploads();

    auto const* const record = resources.quadrics.get_ptr(batch);
    REQUIRE(record != nullptr);
    REQUIRE(record->state == sv::residency::complete);

    auto const& permutation = resources.shaders.acquire_quadric_fallback();

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(sg::tlas_instance{.blas = record->blas, .instance_id = 0, .hit_group_offset = 0});

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(&permutation);

    auto camera = sv::camera::looking_at(tg::pos3d(0, 0.9, 5.0), tg::pos3d(0, 0, 0));
    camera.projection.aspect_ratio = 1.0;

    auto const gpu_camera = sv::camera_gpu::from(camera);

    auto const trace = [&](sg::command_list& cmd, sg::data_future<tg::vec4f>& readback)
    {
        auto const primitives = u32(resources.acquire_buffer(record->primitives.raw()->as_raw_readonly()));

        auto records = cc::vector<sv::instance_gpu>();
        records.push_back(
            {.param_buffer = primitives, .param_offset = 0, .vertices = primitives, .indices = primitives, .is_indexed = 0});

        auto const frame = ctx.transient.create_buffer_from_pod(
            cmd, sv::pt_frame_constants_gpu{.camera = gpu_camera, .samples_per_pixel = 4, .max_bounces = 1},
            sg::buffer_usage::constants_buffer);

        auto const background = ctx.transient.create_buffer_from_pod(
            cmd, sv::background_gpu::from(sv::background::uniform(tg::vec3f(env_radiance, env_radiance, env_radiance))),
            sg::buffer_usage::constants_buffer);

        auto const target = ctx.transient.create_texture_2d(
            {.format = sg::pixel_format::rgba32_float,
             .width = agreement_size,
             .height = agreement_size,
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});

        auto const instance_table
            = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer);

        auto const bindless = resources.freeze();
        auto const outcome = sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                                  .background = background,
                                                                  .instances = instances,
                                                                  .output = target,
                                                                  .instance_table = instance_table,
                                                                  .hit_groups = hit_groups,
                                                                  .bindless = &bindless});

        if (outcome == sg::routine_outcome::executed)
            readback = sg::data_future<tg::vec4f>(cmd.download.bytes_from_texture(target.raw()));

        return outcome;
    };

    auto pixels = cc::vector<tg::vec4f>();
    auto const loop_start = cc::current_time_steady_secs();
    while (pixels.empty())
    {
        (void)ctx.routines.tick();

        auto readback = sg::data_future<tg::vec4f>();
        auto cmd = ctx.create_command_list();
        auto const outcome = trace(*cmd, readback);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();

        if (outcome != sg::routine_outcome::executed)
        {
            REQUIRE(cc::current_time_steady_secs() - loop_start < 45.0);
            sv_test::drive_ambient_work();
            continue;
        }

        REQUIRE(readback.is_valid());
        co_await ctx.idle_completion();

        auto const delivered = readback.try_get_data();
        REQUIRE(delivered.has_value());
        auto const span = delivered.value().span();
        pixels = cc::vector<tg::vec4f>::create_defaulted(span.size());
        for (auto i = isize(0); i < span.size(); ++i)
            pixels[i] = span[i];
    }

    REQUIRE(pixels.size() == isize(agreement_size) * isize(agreement_size));

    // The ray the raygen forms for a pixel's CENTER, mirrored exactly: ndc is [-1, 1] with y down, and the direction is
    // forward + right_scaled * ndc.x - up_scaled * ndc.y.
    auto const ray_for = [&](i32 x, i32 y)
    {
        auto const ndc_x = (float(x) + 0.5f) / float(agreement_size) * 2.0f - 1.0f;
        auto const ndc_y = (float(y) + 0.5f) / float(agreement_size) * 2.0f - 1.0f;

        auto const dir = gpu_camera.forward + gpu_camera.right_scaled * ndc_x - gpu_camera.up_scaled * ndc_y;
        auto const origin = tg::pos3f::zero + gpu_camera.position;
        return tg::ray3f(origin, tg::normalize(dir));
    };

    auto const is_hit = [&](tg::vec4f const& px) { return luminance_of(px) < env_radiance * 0.9f; };

    auto disagreements = isize(0);
    auto cpu_hits = isize(0);

    // One per primitive, so a shape that contributes nothing is visible as a zero rather than hidden in the total.
    // That is what catches a shape placed out of frame or behind another one, which would otherwise make this test
    // quietly cover less than it says it does.
    auto per_primitive = cc::vector<isize>::create_defaulted(set.primitive_count());

    // Every hit must also lie in the primitive's own box, which is the invariant the whole design rests on: the box is
    // the acceleration structure's, so a solid reaching outside it is geometry the GPU can never be asked about.
    // Checked here rather than in a unit test because this is the one place a hand-built primitive meets a real trace.
    auto outside_bounds = isize(0);

    for (auto y = 0; y < agreement_size; ++y)
        for (auto x = 0; x < agreement_size; ++x)
        {
            auto const ray = ray_for(x, y);

            auto cpu = false;
            for (auto i = isize(0); i < set.primitive_count(); ++i)
            {
                auto const& prim = set.primitives()[i];
                auto const hit = sv::intersect(prim, ray, 0.0f);
                if (!hit.has_value())
                    continue;

                cpu = true;
                ++per_primitive[i];

                auto const p = ray.origin + ray.dir * hit.value().t;
                auto const& b = prim.bounds;
                constexpr float slack = 1e-3f; // a hit ON the boundary is in it
                for (auto axis = 0; axis < 3; ++axis)
                    if (p[axis] < b.min[axis] - slack || p[axis] > b.max[axis] + slack)
                    {
                        ++outside_bounds;
                        break;
                    }
            }

            if (cpu)
                ++cpu_hits;

            if (cpu != is_hit(pixels[y * agreement_size + x]))
                ++disagreements;
        }

    // Every shape has to cover REAL area, not one grazing pixel, or this covers fewer of them than it claims.
    // Sixty pixels is about an 8x8 patch, which is the floor at which a silhouette is a shape rather than a speck.
    for (auto i = isize(0); i < per_primitive.size(); ++i)
        CHECK(per_primitive[i] > 60);

    // A solid outside its own box is a hole the GPU cannot fill, and it is the mistake a hand-built primitive makes.
    CHECK(outside_bounds == 0);

    // The shapes have to be in frame at all, or agreeing about an empty image would prove nothing.
    CHECK(cpu_hits > pixels.size() / 12);

    // Measured at 2314 hits over the seven shapes, 73 of them disagreeing — about 3%, which is the perimeter and
    // nothing else.
    // The smallest shape covers 138 pixels, so none of them is a speck the check would pass by accident.
    CC_LOG_INFO("quadric agreement: {} cpu hits over {} shapes, {} disagreements, {} outside bounds", cpu_hits,
                set.primitive_count(), disagreements, outside_bounds);

    // A pixel is one sample of an area: the GPU jitters four inside it while the CPU takes the center, so the two can only
    // differ where the silhouette crosses the pixel.
    //
    // A tenth is the bound because it leaves the perimeter room while still failing on anything structural: a shader that
    // hit nothing would disagree on every hit, and losing one of the seven shapes — or a cylinder's cap, or the
    // hemisphere's floor — is hundreds.
    CHECK(disagreements < cpu_hits / 10);

    // Everything this test set going, including what the routine started on the frame path.
    // `sv::background_work` is the context-wide answer, and that is the point: a trace kicks off compiles nothing
    // here holds a handle to, so naming individual nodes cannot cover them.
    co_await cc::async_settled(sv::background_work(ctx));
}

ASYNC_INVOCABLE_TEST("sv - a quadric batch still draws while its own permutation is compiling",
                     (sg::context_handle const& ctx_h))
{
    // The substitution path, which every other test here deliberately avoids by draining its compiles first.
    //
    // A permutation that has not compiled is stood in for by a neutral one, so a material still building costs its own
    // geometry its shading rather than costing the view its image.
    // For a quadric the stand-in has to be the PROCEDURAL one: a procedural BLAS traced by a hit group with no
    // intersection shader reports no hits, so substituting the triangle fallback would make the batch vanish instead.
    //
    // What this pins is that it does not vanish.
    // The batch is traced before anything has driven its own permutation, and the silhouette has to be there anyway.
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif

    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the quadric shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);

    auto set = sv::quadric_set();
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));

    auto const batch = resources.quadrics.acquire(sv::quadric_data::of(set));
    resources.wait_for_pending_uploads();

    auto const* const record = resources.quadrics.get_ptr(batch);
    REQUIRE(record != nullptr);
    REQUIRE(record->state == sv::residency::complete);

    // A material whose body DOES NOT COMPILE, which is the deterministic way to reach the substitution branch.
    //
    // "Still compiling" was the obvious premise and it is not reproducible: the shader cache is content-addressed and
    // process-wide, so the second run of this test — or any earlier test that compiled the same permutation — hands back
    // a node that already has its value, and there is nothing left to stand in for.
    // A failed compile stays failed however warm the cache is, and `_variant_for` treats the two the same way: its own
    // comment is "still compiling, or a material that does not build".
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("tint", tg::vec3f(0.9f, 0.2f, 0.1f)));
    auto const type = sv::material_type::create("sv_test_uncompilable", cc::move(signature),
                                                "    surface.base_color = tint;\n"
                                                "    this_function_does_not_exist(surface);");
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    auto resident = sv::resident_quadric_set{.name = "one sphere", .geometry = batch, .primitive_count = 1};
    auto const resolved = sv::resolve_material(type, material, resident);
    auto const* const own = &resources.shaders.acquire_quadric(resolved);

    // The premise: it settles without a value, so the trace has to substitute.
    co_await cc::async_settled(own->shader);
    REQUIRE(own->shader->has_error());
    REQUIRE(own->shader->try_value() == nullptr);

    // The stand-in the trace would pick, and the one thing that makes it usable here: it is procedural.
    auto const& stand_in = resources.shaders.acquire_quadric_fallback();
    REQUIRE(stand_in.intersection.is_valid());

    co_await cc::async_settled(stand_in.shader);
    co_await cc::async_settled(stand_in.intersection);
    REQUIRE(stand_in.shader->try_value() != nullptr);

    auto hit_groups = cc::vector<sv::material_permutation const*>();
    hit_groups.push_back(own);

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back(
        sg::tlas_instance{.blas = record->blas, .instance_id = 0, .hit_group_offset = 0, .opaque_override = true});

    auto camera = sv::camera::looking_at(tg::pos3d(0, 0, 4.0), tg::pos3d(0, 0, 0));
    camera.projection.aspect_ratio = 1.0;
    auto const gpu_camera = sv::camera_gpu::from(camera);

    auto const trace = [&](sg::command_list& cmd, sg::data_future<tg::vec4f>& readback)
    {
        auto const primitives = u32(resources.acquire_buffer(record->primitives.raw()->as_raw_readonly()));

        auto records = cc::vector<sv::instance_gpu>();
        records.push_back(
            {.param_buffer = primitives, .param_offset = 0, .vertices = primitives, .indices = primitives, .is_indexed = 0});

        auto const frame = ctx.transient.create_buffer_from_pod(
            cmd, sv::pt_frame_constants_gpu{.camera = gpu_camera, .samples_per_pixel = 2, .max_bounces = 1},
            sg::buffer_usage::constants_buffer);

        auto const background = ctx.transient.create_buffer_from_pod(
            cmd, sv::background_gpu::from(sv::background::uniform(tg::vec3f(env_radiance, env_radiance, env_radiance))),
            sg::buffer_usage::constants_buffer);

        auto const target = ctx.transient.create_texture_2d(
            {.format = sg::pixel_format::rgba32_float,
             .width = image_size,
             .height = image_size,
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});

        auto const instance_table
            = ctx.transient.create_buffer_from_data(cmd, records, sg::buffer_usage::readonly_buffer);

        auto const bindless = resources.freeze();
        auto const outcome = sv::pathtrace_routine::execute(cmd, {.frame = frame,
                                                                  .background = background,
                                                                  .instances = instances,
                                                                  .output = target,
                                                                  .instance_table = instance_table,
                                                                  .hit_groups = hit_groups,
                                                                  .fallback = &resources.shaders.acquire_fallback(),
                                                                  .quadric_fallback = &stand_in,
                                                                  .bindless = &bindless});

        if (outcome == sg::routine_outcome::executed)
            readback = sg::data_future<tg::vec4f>(cmd.download.bytes_from_texture(target.raw()));

        return outcome;
    };

    auto covered = isize(0);
    auto const loop_start = cc::current_time_steady_secs();
    auto traced = false;
    while (!traced)
    {
        (void)ctx.routines.tick();

        auto readback = sg::data_future<tg::vec4f>();
        auto cmd = ctx.create_command_list();
        auto const outcome = trace(*cmd, readback);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();

        if (outcome != sg::routine_outcome::executed)
        {
            REQUIRE(cc::current_time_steady_secs() - loop_start < 45.0);
            sv_test::drive_ambient_work();
            continue;
        }

        REQUIRE(readback.is_valid());
        co_await ctx.idle_completion();

        auto const delivered = readback.try_get_data();
        REQUIRE(delivered.has_value());
        for (auto const& px : delivered.value().span())
            if (luminance_of(px) < env_radiance * 0.9f)
                ++covered;

        traced = true;
    }

    // The sphere is there.
    // Stood in for by the TRIANGLE fallback instead, this is 0: the procedural BLAS would be traversed by a hit group
    // with no intersection shader and report nothing at all.
    auto const expected = covered_fraction(1.0, 4.0, 60.0) * double(image_size) * double(image_size);
    CHECK(double(covered) > expected * 0.5);

    // Drained so nothing this test started outlives it; both settled on the error rather than on a value.
    co_await cc::async_settled(own->intersection);

    // Everything this test set going, including what the routine started on the frame path.
    // `sv::background_work` is the context-wide answer, and that is the point: a trace kicks off compiles nothing
    // here holds a handle to, so naming individual nodes cannot cover them.
    co_await cc::async_settled(sv::background_work(ctx));
}
