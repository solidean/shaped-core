#pragma once

#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/rendering/shaders.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/pbr_material.hh>
#include <shaped-viewer/shader_library.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv_test
{
struct env;
struct rng;
struct triangle_cloud;
struct indexed_mesh;
struct area_light;
struct cornell_box;
} // namespace sv_test

namespace sv_test
{
struct env;
struct rng;
struct triangle_cloud;
struct indexed_mesh;
struct area_light;
struct cornell_box;
} // namespace sv_test

// Shared setup for shaped-viewer's GPU tests (Windows + DXC only).
//
// The generated shader symbols are process-wide globals, and a package may be registered only once per process.
// So the shader library is created exactly once for the whole test binary, here, and every test that traces goes through it.

namespace sv_test
{
using namespace cc::primitive_defines;

} // namespace sv_test

struct sv_test::env
{
    slib::shader_library* lib = nullptr; // intentionally leaked: process-wide, lives for the test binary
    bool has_compiler = false;
};

namespace sv_test
{

/// The one shader library for this test binary — sv's own, reached through the same hook a viewer uses.
/// `has_compiler` is false when DXC is not installed — a caller SKIPs, since nothing will compile.
///
/// Built by `sv::impl::acquire_default_shader_library`, so a test compiles through exactly what a viewer compiles through rather
/// than through a second library assembled to look like it.
inline env const& shared_env()
{
    static env const e = []
    {
        auto lib = sv::acquire_shader_library();
        CC_ASSERT(lib.has_value(), "the default shader library must come up for the GPU tests");
        return env{.lib = lib.value(),
                   .has_compiler = lib.value()->can_compile(slib::shader_language::hlsl, sg::shader_format::dxil)};
    }();
    return e;
}

/// Give whatever the last frame started somewhere to run.
///
/// **Not just a yield.** A trace kicks its material compiles off with `cc::async_start`, which hands them to the
/// ambient scheduler — and under `SC_THREADS=OFF` that scheduler has no threads of its own, so the only thread that
/// can run them is this one.
/// A loop that only yielded would spin until its deadline against work nobody will ever pick up, which is exactly
/// how the single-threaded preset timed out.
inline void drive_ambient_work()
{
    if (cc::ambient_async_scheduler().try_run_one())
        return;
    if (cc::thread_pump_all())
        return;
    cc::this_thread_yield();
}

/// Drives `ctx.routines.tick()` until `ready()` holds, or until `timeout_secs` elapses; true when it came up.
///
/// **A workaround, and marked as one.** A routine's shaders and pipelines build on the ambient async scheduler, off
/// the calling thread, and a path-traced view additionally waits on one compile per material permutation.
/// So a test that drives a fixed number of frames and then asserts is really asserting on compile latency, which is
/// the machine's rather than the code's.
///
/// The real answer is an `ASYNC_TEST` that simply `co_await`s readiness.
/// That needs main-thread affinity for `cc::async`, which clean-core does not have yet -- it is recorded in
/// libs/graphics/shaped-graphics/docs/TODO.md under the ASYNC_TEST migration.
/// Every sv test that needs a routine up goes through here until then, so the places to revisit are exactly this
/// function's callers.
template <class F>
[[nodiscard]] bool tick_until(sg::context& ctx, F&& ready, double timeout_secs = 30.0)
{
    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        (void)ctx.routines.tick();
        if (ready())
            return true;
        if (cc::current_time_steady_secs() - start >= timeout_secs)
            return false;
        drive_ambient_work();
    }
}

/// Records and submits `body(cmd)` as a whole frame, over and over, until it reports `executed`.
///
/// **The same workaround as tick_until, one level up**, and the one most sv tests need.
/// A path trace is not usable the moment its routine reports ready: it still needs one compile per material
/// permutation (or the fallback), and then a DXR state object that is built asynchronously and polled across frames.
/// So "record one frame and assert it traced" is asserting that all of that finished within one frame, which is a
/// statement about the machine.
///
/// Each iteration ticks, records, submits, advances and drains — a real frame — so whatever the previous one started
/// has somewhere to land.
/// Replaced by an ASYNC_TEST that co_awaits readiness once cc::async has main-thread affinity; see
/// libs/graphics/shaped-graphics/docs/TODO.md.
template <class F>
[[nodiscard]] bool frames_until_executed(sg::context& ctx, F&& body, double timeout_secs = 60.0)
{
    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        (void)ctx.routines.tick();

        auto cmd = ctx.create_command_list();
        auto const outcome = body(*cmd);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        ctx.block_until_idle();

        if (outcome == sg::routine_outcome::executed)
            return true;
        if (cc::current_time_steady_secs() - start >= timeout_secs)
            return false;
        drive_ambient_work();
    }
}

} // namespace sv_test

/// A tiny deterministic LCG, so a cloud is reproducible across runs (headless test) yet varied.
struct sv_test::rng
{
    u64 state;
    explicit rng(u64 seed) : state(seed != 0 ? seed : 1) {}

    float unit()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return float((state >> 40) & 0xFFFFFFu) / float(0xFFFFFFu);
    }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

struct sv_test::triangle_cloud
{
    cc::vector<tg::pos3f> positions;        // non-indexed triangle list (3 per triangle)
    cc::vector<sv::pbr_material> materials; // one per triangle
};

namespace sv_test
{

/// A random cloud of `triangle_count` small triangles scattered in a box, each with its own PBR material.
inline triangle_cloud make_triangle_cloud(int triangle_count, u64 seed = 0x5EED1234u)
{
    auto out = triangle_cloud{};
    auto r = rng(seed);

    for (auto i = 0; i < triangle_count; ++i)
    {
        auto const center = tg::pos3f(r.range(-1.6f, 1.6f), r.range(-1.6f, 1.6f), r.range(-1.6f, 1.6f));
        auto const spread = 0.35f;
        for (auto k = 0; k < 3; ++k)
            out.positions.push_back(tg::pos3f(center[0] + r.range(-spread, spread), center[1] + r.range(-spread, spread),
                                              center[2] + r.range(-spread, spread)));

        out.materials.push_back({.base_color = tg::vec3f(r.range(0.05f, 1.0f), r.range(0.05f, 1.0f), r.range(0.05f, 1.0f)),
                                 .metallic = r.range(0.0f, 1.0f),
                                 .roughness = r.range(0.08f, 1.0f),
                                 .emissive = tg::vec3f(0, 0, 0)});
    }
    return out;
}

} // namespace sv_test

namespace sv_test
{

/// The material library the tests author through — the process-wide one a viewer draws from.
///
/// Reached through `sv::acquire_material_library`, so a test resolves against exactly what the render path resolves
/// against rather than against a second library assembled to look like it.
inline sv::material_library& shared_material_library()
{
    auto lib = sv::acquire_material_library();
    CC_ASSERT(lib.has_value(), "the default material library must come up for the tests");
    return *lib.value();
}

/// The per-face values of `materials`, as the four `per_triangle` attributes the builtin `pbr` type declares by name.
/// One attribute per field, because a `mesh_attribute` carries a scalar or a vector rather than a struct.
inline cc::vector<sv::mesh_attribute> pbr_face_attributes(cc::span<sv::pbr_material const> materials)
{
    auto base_color = cc::vector<tg::vec3f>();
    auto metallic = cc::vector<f32>();
    auto roughness = cc::vector<f32>();
    auto emissive = cc::vector<tg::vec3f>();

    for (auto const& m : materials)
    {
        base_color.push_back(m.base_color);
        metallic.push_back(m.metallic);
        roughness.push_back(m.roughness);
        emissive.push_back(m.emissive);
    }

    auto const frequency = sv::attribute_frequency::per_triangle;

    auto out = cc::vector<sv::mesh_attribute>();
    out.push_back(sv::mesh_attribute::create("base_color", frequency, cc::move(base_color)));
    out.push_back(sv::mesh_attribute::create("metallic", frequency, cc::move(metallic)));
    out.push_back(sv::mesh_attribute::create("roughness", frequency, cc::move(roughness)));
    out.push_back(sv::mesh_attribute::create("emissive", frequency, cc::move(emissive)));
    return out;
}

/// A raw triangle list plus its per-face materials, as the `sv::mesh` the authoring API takes.
///
/// The mesh names the library's unbound `pbr` material, so each of the four attributes above wins over the type's own
/// default and the generated closest-hit reads them per triangle.
inline sv::mesh as_mesh(cc::string name, cc::span<tg::pos3f const> positions, cc::span<sv::pbr_material const> materials)
{
    return {.name = cc::move(name),
            .geometry = sv::triangle_geometry::create_from_positions(positions),
            .attributes = pbr_face_attributes(materials),
            .material = sv::default_material(shared_material_library())};
}

/// The same geometry, shaded through a TEXTURE rather than through per-face colours.
///
/// This is the only shape of scene that reaches the path tracer's sampler group: a permutation declares a sampler
/// only when its material samples something, and that sampler group is a third `binding_group_layout` the routine
/// builds and a third slot in its pipeline layout.
/// Every other scene here is untextured, so without this the whole of that path is unreached.
///
/// `base_color` is bound as a texture INSTEAD of as a per-face attribute, so nothing fills the same slot twice.
inline sv::mesh as_textured_mesh(cc::string name, cc::span<tg::pos3f const> positions)
{
    // One uv per vertex.
    // The values decide what the sample reads, and nothing here checks the pixels, so they only have to exist.
    auto uvs = cc::array<tg::vec2f>::create_defaulted(positions.size());
    for (auto i = isize(0); i < uvs.size(); ++i)
        uvs[i] = tg::vec2f(f32(i % 2), f32((i / 2) % 2));

    auto attributes = cc::vector<sv::mesh_attribute>();
    attributes.push_back(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, cc::move(uvs)));

    auto mesh = sv::mesh{.name = cc::move(name),
                         .geometry = sv::triangle_geometry::create_from_positions(positions),
                         .attributes = cc::move(attributes),
                         .material = sv::default_material(shared_material_library())};

    // A 2x2 opaque white texture: enough to mint an id and a sampler, and neutral enough that the image is the
    // one the untextured sibling produces.
    auto pixels = cc::array<byte>::create_filled(2 * 2 * 4, byte(255));
    mesh.textures.push_back(
        {.name = "base_color",
         .source = {.texture = sv::texture_data::create(cc::move(pixels), sg::pixel_format::rgba8_unorm, 2, 2)}});
    return mesh;
}

/// The same, over indexed geometry — triangle order follows the index buffer, so the per-face attributes still line up.
inline sv::mesh as_indexed_mesh(cc::string name,
                                cc::span<tg::pos3f const> positions,
                                cc::span<u32 const> indices,
                                cc::span<sv::pbr_material const> materials)
{
    return {.name = cc::move(name),
            .geometry = sv::triangle_geometry::create_from_indexed_triangles(positions, indices),
            .attributes = pbr_face_attributes(materials),
            .material = sv::default_material(shared_material_library())};
}

} // namespace sv_test

/// A welded indexed triangle list: the same geometry as its source, with duplicate positions collapsed onto
/// one vertex — so the index buffer genuinely shares vertices (both triangles of a quad land on 4 positions).
struct sv_test::indexed_mesh
{
    cc::vector<tg::pos3f> positions;
    cc::vector<u32> indices;
};

namespace sv_test
{

/// Welds `triangle_list` (3 positions per triangle) into an indexed_mesh, preserving triangle order — so a material set indexed by PrimitiveIndex() still lines up.
/// O(n²): test-sized meshes only.
inline indexed_mesh weld_triangle_list(cc::span<tg::pos3f const> triangle_list)
{
    auto out = indexed_mesh{};
    for (auto const& p : triangle_list)
    {
        auto index = out.positions.size(); // stays == size() while p is unseen
        for (auto i = isize(0); i < out.positions.size(); ++i)
            if (out.positions[i] == p)
            {
                index = i;
                break;
            }
        if (index == out.positions.size())
            out.positions.push_back(p);
        out.indices.push_back(u32(index));
    }
    return out;
}

} // namespace sv_test

/// The rectangular ceiling light of a Cornell box.
/// The geometry (a quad in `positions`) is what emits.
/// These fields let the caller fill the path tracer's pt_frame_constants_gpu so its next-event estimation samples the exact same rectangle.
struct sv_test::area_light
{
    tg::vec3f center;   // rect center in world space (on the ceiling plane)
    float half_x;       // half-extent along world x
    float half_z;       // half-extent along world z
    tg::vec3f emission; // emitted radiance (matches the emissive of the light material)
};

/// A Cornell box as a non-indexed triangle list with per-triangle materials, plus its light description.
struct sv_test::cornell_box
{
    cc::vector<tg::pos3f> positions;        // non-indexed triangle list (3 per triangle)
    cc::vector<sv::pbr_material> materials; // one per triangle
    area_light light;
};

namespace sv_test
{

/// Appends a quad (a-b-c + a-c-d) carrying material `m` to a Cornell box.
/// Winding is irrelevant — the closest-hit shades two-sided.
inline void cb_push_quad(cornell_box& cb, tg::pos3f a, tg::pos3f b, tg::pos3f c, tg::pos3f d, sv::pbr_material const& m)
{
    cb.positions.push_back(a);
    cb.positions.push_back(b);
    cb.positions.push_back(c);
    cb.positions.push_back(a);
    cb.positions.push_back(c);
    cb.positions.push_back(d);
    cb.materials.push_back(m);
    cb.materials.push_back(m);
}

/// Appends an axis-aligned box spanning [lo, hi] (six quads) carrying material `m`.
inline void cb_push_box(cornell_box& cb, tg::pos3f lo, tg::pos3f hi, sv::pbr_material const& m)
{
    auto const x0 = lo[0];
    auto const y0 = lo[1];
    auto const z0 = lo[2];
    auto const x1 = hi[0];
    auto const y1 = hi[1];
    auto const z1 = hi[2];

    cb_push_quad(cb, tg::pos3f(x0, y0, z0), tg::pos3f(x1, y0, z0), tg::pos3f(x1, y0, z1), tg::pos3f(x0, y0, z1),
                 m); // bottom
    cb_push_quad(cb, tg::pos3f(x0, y1, z0), tg::pos3f(x0, y1, z1), tg::pos3f(x1, y1, z1), tg::pos3f(x1, y1, z0), m); // top
    cb_push_quad(cb, tg::pos3f(x0, y0, z0), tg::pos3f(x0, y1, z0), tg::pos3f(x1, y1, z0), tg::pos3f(x1, y0, z0), m); // front
    cb_push_quad(cb, tg::pos3f(x0, y0, z1), tg::pos3f(x1, y0, z1), tg::pos3f(x1, y1, z1), tg::pos3f(x0, y1, z1), m); // back
    cb_push_quad(cb, tg::pos3f(x0, y0, z0), tg::pos3f(x0, y0, z1), tg::pos3f(x0, y1, z1), tg::pos3f(x0, y1, z0), m); // left
    cb_push_quad(cb, tg::pos3f(x1, y0, z0), tg::pos3f(x1, y1, z0), tg::pos3f(x1, y1, z1), tg::pos3f(x1, y0, z1), m); // right
}

/// A classic Cornell box in a unit-ish cube centered at the origin, open toward -z (the camera side).
/// White floor / ceiling / back, a red left wall and a green right wall, two white boxes on the floor, and a rectangular emitter just below the ceiling.
/// All geometry is already in world space (identity transform).
inline cornell_box make_cornell_box()
{
    auto const white = sv::pbr_material{.base_color = tg::vec3f(0.73f, 0.73f, 0.73f),
                                        .metallic = 0.0f,
                                        .roughness = 1.0f,
                                        .emissive = tg::vec3f(0, 0, 0)};
    auto const red = sv::pbr_material{.base_color = tg::vec3f(0.63f, 0.06f, 0.05f),
                                      .metallic = 0.0f,
                                      .roughness = 1.0f,
                                      .emissive = tg::vec3f(0, 0, 0)};
    auto const green = sv::pbr_material{.base_color = tg::vec3f(0.12f, 0.45f, 0.09f),
                                        .metallic = 0.0f,
                                        .roughness = 1.0f,
                                        .emissive = tg::vec3f(0, 0, 0)};

    auto const light_emission = tg::vec3f(15, 15, 15);
    // A pure emitter: no reflection (albedo 0), so a path that lands on it terminates on its emission.
    auto const lamp = sv::pbr_material{.base_color = tg::vec3f(0, 0, 0),
                                       .metallic = 0.0f,
                                       .roughness = 1.0f,
                                       .emissive = light_emission};

    auto cb = cornell_box{};

    // enclosure: floor, ceiling, back wall, colored side walls (no front wall — the box opens toward the camera)
    cb_push_quad(cb, tg::pos3f(-1, -1, -1), tg::pos3f(1, -1, -1), tg::pos3f(1, -1, 1), tg::pos3f(-1, -1, 1), white); // floor
    cb_push_quad(cb, tg::pos3f(-1, 1, -1), tg::pos3f(-1, 1, 1), tg::pos3f(1, 1, 1), tg::pos3f(1, 1, -1), white); // ceiling
    cb_push_quad(cb, tg::pos3f(-1, -1, 1), tg::pos3f(1, -1, 1), tg::pos3f(1, 1, 1), tg::pos3f(-1, 1, 1), white); // back
    cb_push_quad(cb, tg::pos3f(-1, -1, -1), tg::pos3f(-1, -1, 1), tg::pos3f(-1, 1, 1), tg::pos3f(-1, 1, -1),
                 red); // left wall
    cb_push_quad(cb, tg::pos3f(1, -1, -1), tg::pos3f(1, 1, -1), tg::pos3f(1, 1, 1), tg::pos3f(1, -1, 1),
                 green); // right wall

    // two white blocks standing on the floor
    cb_push_box(cb, tg::pos3f(-0.55f, -1.0f, -0.15f), tg::pos3f(-0.05f, -0.35f, 0.4f), white); // short box
    cb_push_box(cb, tg::pos3f(0.1f, -1.0f, 0.05f), tg::pos3f(0.6f, 0.25f, 0.6f), white);       // tall box

    // ceiling light: a rectangle just below the ceiling, emitting straight down
    auto const light_y = 0.998f;
    auto const half_x = 0.35f;
    auto const half_z = 0.35f;
    cb_push_quad(cb, tg::pos3f(-half_x, light_y, -half_z), tg::pos3f(half_x, light_y, -half_z),
                 tg::pos3f(half_x, light_y, half_z), tg::pos3f(-half_x, light_y, half_z), lamp);
    cb.light
        = area_light{.center = tg::vec3f(0, light_y, 0), .half_x = half_x, .half_z = half_z, .emission = light_emission};

    return cb;
}
} // namespace sv_test
