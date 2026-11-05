// How fast inline traces run, in rays per second: on hardware natively, and through the ray-query polyfill on webgpu.
//
// The polyfill builds its tree over the primitive order with no spatial sort, and nothing had measured it.
// So the scenes here are chosen to show what that costs: one heightfield in a coherent triangle order, the same
// triangles shuffled into a soup, and the soup sorted again by Morton code, which is what a sorted build would buy.
// An assembly of instanced parts is the shape a CAD scene has.
//
// Three kinds of rays per scene: camera rays (coherent), probe rays leaving probe centers in random directions
// (incoherent, what probe GI traces), and short occlusion rays.
// A small trial dispatch prices a ray, and the measured batches are cut to that price, up to a million rays a dispatch.
// The shuffled soup traces hundreds of times slower than the same triangles in order, which a fixed batch would
// turn into hours and a driver watchdog into a lost device.
// Each batch is timed on the GPU where timestamps exist, by the wall clock otherwise, and printed as a table.
//
// On the web the page shows the table as it fills and the scene being measured; under node or without a window it
// prints the table and exits.
// Nothing here blocks: every wait is awaited, which is what lets one source serve webgpu.

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <ray_throughput_shaders.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/window.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>

#if RAY_THROUGHPUT_BACKEND_WEBGPU
#include <emscripten/em_asm.h>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#elif RAY_THROUGHPUT_BACKEND_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#elif RAY_THROUGHPUT_BACKEND_METAL
#include <shaped-graphics/backends/metal/metal_context.hh>
#else
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#endif

using namespace cc::primitive_defines;

namespace
{
namespace rt = shaders::ray_throughput;

constexpr int camera_width = 1024;
constexpr int camera_height = 1024;
constexpr int probes_x = 16;
constexpr int probes_y = 8;
constexpr int probes_z = 16;
constexpr int rays_per_probe = 512;
constexpr int max_rays_per_dispatch = camera_width * camera_height;
constexpr int max_dispatches_per_batch = 8;
/// What a dispatch and a batch are sized to, so a slow structure measures in seconds rather than hours.
/// **No single dispatch may run long on any device**: a driver resets a GPU whose work outlasts its watchdog, about
/// two seconds on Windows and less on a phone, and an integrated GPU traces a soup thousands of times slower than a
/// sorted tree on a discrete one.
constexpr double target_dispatch_ms = 25.0;
constexpr double target_batch_ms = 300.0;
/// A scene whose first, smallest dispatch takes longer than this is too slow for this device and is skipped.
constexpr double slowest_trial_ms = 100.0;
/// The preview is traced only where the scene's measured camera rays say it fits in this.
constexpr double preview_budget_ms = 30.0;
constexpr int batches = 3;
constexpr int preview_width = 640;
constexpr int preview_height = 360;

[[nodiscard]] float sin_of(float radians)
{
    return tg::sin(tg::angle_f::make_from_radians(radians));
}

[[nodiscard]] float cos_of(float radians)
{
    return tg::cos(tg::angle_f::make_from_radians(radians));
}

/// Positions and an index list, three indices a triangle.
struct mesh_data
{
    cc::vector<tg::pos3f> positions;
    cc::vector<u32> indices;

    [[nodiscard]] isize triangle_count() const { return indices.size() / 3; }

    [[nodiscard]] u32 add_vertex(tg::pos3f p)
    {
        positions.push_back(p);
        return u32(positions.size() - 1);
    }

    void add_triangle(u32 a, u32 b, u32 c)
    {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }
};

/// A gently rolling surface over [-1, 1]² in x and z, `n` vertices a side, its triangles in row order.
[[nodiscard]] mesh_data make_heightfield(int n)
{
    auto mesh = mesh_data();
    for (auto zi = 0; zi < n; ++zi)
        for (auto xi = 0; xi < n; ++xi)
        {
            auto const x = -1.0f + 2.0f * float(xi) / float(n - 1);
            auto const z = -1.0f + 2.0f * float(zi) / float(n - 1);
            auto const y
                = 0.12f * sin_of(5.0f * x) * cos_of(4.0f * z) + 0.04f * sin_of(17.0f * x + 3.0f) * sin_of(13.0f * z);
            (void)mesh.add_vertex(tg::pos3f(x, y, z));
        }
    for (auto zi = 0; zi + 1 < n; ++zi)
        for (auto xi = 0; xi + 1 < n; ++xi)
        {
            auto const a = u32(zi * n + xi);
            auto const b = a + 1;
            auto const c = a + u32(n);
            auto const d = c + 1;
            mesh.add_triangle(a, c, b);
            mesh.add_triangle(b, c, d);
        }
    return mesh;
}

/// The closed box between `lo` and `hi`, 12 triangles: the room every scene stands in, so most rays hit something.
void add_box(mesh_data& mesh, tg::pos3f lo, tg::pos3f hi)
{
    auto corner
        = [&](int i) { return tg::pos3f((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]); };
    auto const base = u32(mesh.positions.size());
    for (auto i = 0; i < 8; ++i)
        (void)mesh.add_vertex(corner(i));
    int const faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (auto const& f : faces)
    {
        mesh.add_triangle(base + u32(f[0]), base + u32(f[1]), base + u32(f[2]));
        mesh.add_triangle(base + u32(f[0]), base + u32(f[2]), base + u32(f[3]));
    }
}

/// A closed cylinder of radius 1 from y = -1 to y = 1, `segments` around.
[[nodiscard]] mesh_data make_cylinder(int segments)
{
    auto mesh = mesh_data();
    auto const top = mesh.add_vertex(tg::pos3f(0, 1, 0));
    auto const bottom = mesh.add_vertex(tg::pos3f(0, -1, 0));
    for (auto i = 0; i < segments; ++i)
    {
        auto const a = 6.2831853f * float(i) / float(segments);
        (void)mesh.add_vertex(tg::pos3f(cos_of(a), 1, sin_of(a)));
        (void)mesh.add_vertex(tg::pos3f(cos_of(a), -1, sin_of(a)));
    }
    for (auto i = 0; i < segments; ++i)
    {
        auto const j = (i + 1) % segments;
        auto const t0 = u32(2 + 2 * i);
        auto const b0 = t0 + 1;
        auto const t1 = u32(2 + 2 * j);
        auto const b1 = t1 + 1;
        mesh.add_triangle(t0, b0, t1);
        mesh.add_triangle(t1, b0, b1);
        mesh.add_triangle(top, t0, t1);
        mesh.add_triangle(bottom, b1, b0);
    }
    return mesh;
}

/// A torus of major radius 1 and minor radius `minor` about the y axis.
[[nodiscard]] mesh_data make_torus(int segments_u, int segments_v, float minor)
{
    auto mesh = mesh_data();
    for (auto i = 0; i < segments_u; ++i)
        for (auto j = 0; j < segments_v; ++j)
        {
            auto const u = 6.2831853f * float(i) / float(segments_u);
            auto const v = 6.2831853f * float(j) / float(segments_v);
            auto const r = 1.0f + minor * cos_of(v);
            (void)mesh.add_vertex(tg::pos3f(r * cos_of(u), minor * sin_of(v), r * sin_of(u)));
        }
    for (auto i = 0; i < segments_u; ++i)
        for (auto j = 0; j < segments_v; ++j)
        {
            auto const i1 = (i + 1) % segments_u;
            auto const j1 = (j + 1) % segments_v;
            auto const a = u32(i * segments_v + j);
            auto const b = u32(i1 * segments_v + j);
            auto const c = u32(i * segments_v + j1);
            auto const d = u32(i1 * segments_v + j1);
            mesh.add_triangle(a, b, c);
            mesh.add_triangle(c, b, d);
        }
    return mesh;
}

/// The same triangles in a random order: the polyfill's worst case, since its tree follows the order.
void shuffle_triangles(mesh_data& mesh, u64 seed)
{
    auto order = cc::vector<u32>::create_defaulted(mesh.triangle_count());
    for (auto i = 0; i < order.size(); ++i)
        order[i] = u32(i);
    auto rng = cc::random(seed);
    rng.shuffle(order);
    auto shuffled = cc::vector<u32>::create_defaulted(mesh.indices.size());
    for (auto i = 0; i < order.size(); ++i)
        for (auto k = 0; k < 3; ++k)
            shuffled[i * 3 + k] = mesh.indices[isize(order[i]) * 3 + k];
    mesh.indices = cc::move(shuffled);
}

/// Spreads the low 10 bits of `v` to every third bit.
[[nodiscard]] u32 spread_bits(u32 v)
{
    auto x = v & 0x3ffu;
    x = (x | (x << 16)) & 0x030000ffu;
    x = (x | (x << 8)) & 0x0300f00fu;
    x = (x | (x << 4)) & 0x030c30c3u;
    x = (x | (x << 2)) & 0x09249249u;
    return x;
}

/// The triangles sorted by the Morton code of their centroids: what a sorted build would hand the polyfill.
void morton_sort_triangles(mesh_data& mesh)
{
    auto lo = tg::pos3f(1e30f, 1e30f, 1e30f);
    auto hi = tg::pos3f(-1e30f, -1e30f, -1e30f);
    for (auto const& p : mesh.positions)
        for (auto a = 0; a < 3; ++a)
        {
            lo[a] = cc::min(lo[a], p[a]);
            hi[a] = cc::max(hi[a], p[a]);
        }

    struct keyed
    {
        u32 code = 0;
        u32 triangle = 0;
    };
    auto keys = cc::vector<keyed>::create_defaulted(mesh.triangle_count());
    for (auto t = 0; t < keys.size(); ++t)
    {
        auto c = tg::vec3f(0, 0, 0);
        for (auto k = 0; k < 3; ++k)
        {
            auto const p = mesh.positions[isize(mesh.indices[t * 3 + k])];
            c = c + tg::vec3f(p[0], p[1], p[2]);
        }
        u32 q[3] = {};
        for (auto a = 0; a < 3; ++a)
        {
            auto const extent = cc::max(hi[a] - lo[a], 1e-12f);
            auto const f = (c[a] / 3.0f - lo[a]) / extent;
            q[a] = u32(cc::clamp(f, 0.0f, 1.0f) * 1023.0f);
        }
        keys[t] = {.code = spread_bits(q[0]) | (spread_bits(q[1]) << 1) | (spread_bits(q[2]) << 2), .triangle = u32(t)};
    }
    cc::sort_by(keys, &keyed::code);

    auto sorted = cc::vector<u32>::create_defaulted(mesh.indices.size());
    for (auto i = 0; i < keys.size(); ++i)
        for (auto k = 0; k < 3; ++k)
            sorted[i * 3 + k] = mesh.indices[isize(keys[i].triangle) * 3 + k];
    mesh.indices = cc::move(sorted);
}

/// A scene as uploaded: what it is called, how large it is, and the structure to trace.
struct built_scene
{
    cc::string name;
    isize triangles = 0;
    isize instances = 0;
    double estimated_pool_mib = 0.0;
    tg::pos3f bounds_min = tg::pos3f(-1.2f, -0.5f, -1.2f);
    tg::pos3f bounds_max = tg::pos3f(1.2f, 1.0f, 1.2f);
    cc::vector<sg::raw_buffer_handle> inputs;
    cc::vector<sg::blas_handle> blases;
    sg::tlas_handle tlas;
};

/// The polyfill's size for a BLAS of `triangles`: three units a triangle, two a node, leaves of four, 16-byte units.
[[nodiscard]] double polyfill_blas_bytes(isize triangles)
{
    auto const leaves = (triangles + 3) / 4;
    auto const nodes = 2 * leaves;
    return double(16 * (1 + 3 * triangles + 2 * nodes));
}

[[nodiscard]] sg::blas_handle build_mesh_blas(sg::context& ctx,
                                              sg::command_list& cmd,
                                              mesh_data const& mesh,
                                              built_scene& scene)
{
    auto const usage = sg::buffer_usage::accel_structure_build_input;
    auto const vertices = ctx.persistent.create_buffer_from_data(cc::span<tg::pos3f const>(mesh.positions), usage);
    auto const indices = ctx.persistent.create_buffer_from_data(cc::span<u32 const>(mesh.indices), usage);
    auto const geometry = sg::blas_triangles{.vertices = vertices.raw(),
                                             .vertex_count = mesh.positions.size(),
                                             .indices = indices.raw(),
                                             .index_count = mesh.indices.size(),
                                             .is_opaque = true};
    auto const blas = cmd.raytracing.build_blas(cc::span<sg::blas_triangles const>(&geometry, 1));
    scene.inputs.push_back(vertices.raw());
    scene.inputs.push_back(indices.raw());
    scene.blases.push_back(blas);
    scene.triangles += mesh.triangle_count();
    scene.estimated_pool_mib += polyfill_blas_bytes(mesh.triangle_count()) / (1024.0 * 1024.0);
    return blas;
}

/// A row-major 3x4 transform: a turn about y by `angle`, a uniform `scale`, then a move to `at`.
void place(sg::tlas_instance& instance, float angle, float scale, tg::pos3f at)
{
    auto const c = cos_of(angle) * scale;
    auto const s = sin_of(angle) * scale;
    float const m[12] = {c, 0, s, at[0], 0, scale, 0, at[1], -s, 0, c, at[2]};
    for (auto i = 0; i < 12; ++i)
        instance.transform[i] = m[i];
}

enum class triangle_order
{
    coherent,
    shuffled,
    morton_sorted,
};

/// One heightfield of about `2 (n - 1)²` triangles inside the room, in the order asked for.
[[nodiscard]] built_scene build_heightfield_scene(sg::context& ctx, int n, triangle_order order)
{
    auto mesh = make_heightfield(n);
    add_box(mesh, tg::pos3f(-1.2f, -0.5f, -1.2f), tg::pos3f(1.2f, 1.0f, 1.2f));
    auto const* order_name = "coherent";
    if (order != triangle_order::coherent)
    {
        shuffle_triangles(mesh, 0x5eedu);
        order_name = "soup";
    }
    if (order == triangle_order::morton_sorted)
    {
        morton_sort_triangles(mesh);
        order_name = "soup, Morton-sorted";
    }

    auto scene = built_scene();
    scene.name = cc::format("heightfield {}k, {}", mesh.triangle_count() / 1000, order_name);
    auto cmd = ctx.create_command_list();
    auto const blas = build_mesh_blas(ctx, *cmd, mesh, scene);
    sg::tlas_instance const instance = {.blas = blas, .instance_id = 0, .cull_mode = sg::instance_cull_mode::none};
    scene.tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(&instance, 1));
    scene.instances = 1;
    ctx.submit_command_list(cc::move(cmd));
    return scene;
}

/// Parts on a grid in the room, each turned and sized at random: the shape of a CAD assembly.
[[nodiscard]] built_scene build_assembly_scene(sg::context& ctx, int per_side, int layers)
{
    auto scene = built_scene();
    auto cmd = ctx.create_command_list();

    auto room = mesh_data();
    add_box(room, scene.bounds_min, scene.bounds_max);
    auto const room_blas = build_mesh_blas(ctx, *cmd, room, scene);
    sg::blas_handle const parts[] = {
        build_mesh_blas(ctx, *cmd, make_cylinder(48), scene),
        build_mesh_blas(ctx, *cmd, make_torus(64, 32, 0.35f), scene),
        build_mesh_blas(ctx, *cmd, make_heightfield(24), scene),
    };
    isize const part_triangles[] = {make_cylinder(48).triangle_count(), make_torus(64, 32, 0.35f).triangle_count(),
                                    make_heightfield(24).triangle_count()};

    auto instances = cc::vector<sg::tlas_instance>();
    instances.push_back({.blas = room_blas, .instance_id = 0, .cull_mode = sg::instance_cull_mode::none});
    auto rng = cc::random(0xa55e3b1u);
    auto placed_triangles = isize(0);
    for (auto layer = 0; layer < layers; ++layer)
        for (auto zi = 0; zi < per_side; ++zi)
            for (auto xi = 0; xi < per_side; ++xi)
            {
                auto const part = rng.uniform(0, 2);
                auto instance = sg::tlas_instance{.blas = parts[part],
                                                  .instance_id = u32(instances.size()),
                                                  .cull_mode = sg::instance_cull_mode::none};
                auto const x = -1.0f + 2.0f * (float(xi) + 0.5f) / float(per_side);
                auto const z = -1.0f + 2.0f * (float(zi) + 0.5f) / float(per_side);
                auto const y = -0.35f + 1.1f * (float(layer) + 0.5f) / float(layers);
                place(instance, rng.uniform(0.0f, 6.2831853f), 0.6f / float(per_side), tg::pos3f(x, y, z));
                instances.push_back(instance);
                placed_triangles += part_triangles[part];
            }
    scene.tlas = cmd->raytracing.build_tlas(cc::span<sg::tlas_instance const>(instances));
    scene.instances = instances.size();
    scene.estimated_pool_mib += double(16 * (1 + 7 * instances.size() + 2 * instances.size())) / (1024.0 * 1024.0);
    scene.name = cc::format("assembly, {} instances, {}k triangles placed", instances.size(), placed_triangles / 1000);
    ctx.submit_command_list(cc::move(cmd));
    return scene;
}

enum class ray_kind
{
    camera,
    probe,
    occlusion,
};

[[nodiscard]] char const* name_of(ray_kind kind)
{
    switch (kind)
    {
    case ray_kind::camera:
        return "camera";
    case ray_kind::probe:
        return "probe";
    case ray_kind::occlusion:
        return "occlusion";
    }
    return "";
}

/// The constants every kernel reads, for a camera grid of `width` × `height` rays, which is also the ray count.
[[nodiscard]] rt::params params_for(built_scene const& scene, int width, int height, u32 seed)
{
    auto const lo = scene.bounds_min;
    auto const hi = scene.bounds_max;
    auto const diagonal = (hi - lo).length();
    auto const eye = tg::pos3f(0.0f, 0.75f, -1.15f);
    auto const target = tg::pos3f(0.0f, -0.1f, 0.25f);
    auto const forward = (target - eye).normalized();
    auto const right = tg::dual(tg::cross(tg::vec3f(0, 1, 0), forward)).normalized();
    auto const up = tg::dual(tg::cross(forward, right));
    auto const tan_half = 0.62f; // about 64° vertically
    return rt::params{
        .bounds_min = tg::vec4f(lo[0], lo[1], lo[2], diagonal),
        .bounds_max = tg::vec4f(hi[0], hi[1], hi[2], 0.0f),
        .eye = tg::vec4f(eye[0], eye[1], eye[2], tan_half),
        .forward = tg::vec4f(forward[0], forward[1], forward[2], float(width) / float(height)),
        .right = tg::vec4f(right[0], right[1], right[2], 0.0f),
        .up = tg::vec4f(up[0], up[1], up[2], 0.0f),
        .probe_grid = tg::vec4i(probes_x, probes_y, probes_z, rays_per_probe),
        .ray_count = u32(width * height),
        .width = u32(width),
        .seed = seed,
    };
}

/// The kernels and the layouts they bind.
struct kernels
{
    sg::compute_pipeline_handle camera;
    sg::compute_pipeline_handle probe;
    sg::compute_pipeline_handle occlusion;
    sg::compute_pipeline_handle preview;
    sg::binding_group_layout_handle scene_layout;
    sg::binding_group_layout_handle preview_layout;
};

[[nodiscard]] sg::compute_pipeline_handle const& kernel_of(kernels const& k, ray_kind kind)
{
    if (kind == ray_kind::camera)
        return k.camera;
    if (kind == ray_kind::probe)
        return k.probe;
    return k.occlusion;
}

/// What one batch measured: its GPU time where timestamps exist, and the wall clock around it always.
struct batch_time
{
    double gpu_ms = -1.0;
    double wall_ms = 0.0;
};

/// How a batch is cut: dispatches of a square camera grid `width` rays wide, which is `width²` rays of any kind.
/// `width` is a power of two from 16 to 1024; a probe's rays are consecutive, so a small grid traces part of one probe.
struct batch_shape
{
    int width = 16;
    int dispatches = 1;

    [[nodiscard]] int rays_per_dispatch() const { return width * width; }
    [[nodiscard]] double rays() const { return double(rays_per_dispatch()) * double(dispatches); }
};

/// The largest shape whose dispatches stay near `target_dispatch_ms` and whose batch stays near `target_batch_ms`,
/// for rays that cost `ms_per_ray` each.
[[nodiscard]] batch_shape shape_for(double ms_per_ray)
{
    auto shape = batch_shape();
    while (shape.width < camera_width && double(4 * shape.rays_per_dispatch()) * ms_per_ray <= target_dispatch_ms)
        shape.width *= 2;
    auto const dispatch_ms = double(shape.rays_per_dispatch()) * ms_per_ray;
    shape.dispatches = cc::clamp(int(target_batch_ms / cc::max(dispatch_ms, 1e-3)), 1, max_dispatches_per_batch);
    return shape;
}

/// Traces one batch of one kind, cut as `shape` says, and waits for it.
[[nodiscard]] cc::shared_async<batch_time> time_batch(sg::context_handle ctx,
                                                      kernels const& k,
                                                      built_scene const& scene,
                                                      sg::buffer<u32> const& results,
                                                      ray_kind kind,
                                                      batch_shape shape,
                                                      u32 seed)
{
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, k.scene_layout, rt::scene{.world = scene.tlas->as_view(), .results = results.as_readwrite_buffer()});
    auto const before = cmd->query.record_gpu_timestamp();
    cmd->compute.bind_pipeline(*kernel_of(k, kind));
    cmd->compute.bind_group(0, *group);
    for (auto i = 0; i < shape.dispatches; ++i)
    {
        cmd->compute.set_inline_constants(params_for(scene, shape.width, shape.width, seed + u32(i)).to_block());
        cmd->compute.dispatch_threads(shape.rays_per_dispatch(), 1, 1);
    }
    auto const after = cmd->query.record_gpu_timestamp();

    auto const start = cc::current_time_steady_secs();
    ctx->submit_command_list(cc::move(cmd));
    ctx->advance_epoch();
    co_await ctx->idle_completion();
    auto out = batch_time{.wall_ms = (cc::current_time_steady_secs() - start) * 1000.0};

    if (after.is_valid())
    {
        co_await after.completion();
        auto const t0 = before.try_get_seconds();
        auto const t1 = after.try_get_seconds();
        if (t0.has_value() && t1.has_value())
            out.gpu_ms = (t1.value() - t0.value()) * 1000.0;
    }
    co_return out;
}

/// Where the preview goes: a window, a canvas, or nowhere.
struct display
{
    cc::unique_ptr<sr::window_system> wsys;
    cc::unique_ptr<sr::window> win;
    sg::swapchain_handle swapchain;
    sg::raster_pipeline_handle blit;
    sg::binding_group_layout_handle shown_layout;

    [[nodiscard]] bool is_open() const { return swapchain != nullptr; }
};

/// A window natively, the page's canvas on the web; none under node or without a window backend, which is no error.
[[nodiscard]] cc::shared_async<cc::unit> open_display(sg::context_handle ctx, display& out)
{
    auto const format = sg::pixel_format::bgra8_unorm;
    auto window = sg::native_window();
#ifdef __EMSCRIPTEN__
    // Under node there is no page, and asking for its canvas throws rather than failing.
    // `==` rather than `!==`, which clang-format splits into `!= =`.
    if (EM_ASM_INT({ return typeof document == 'undefined' ? 0 : 1; }) == 0)
        co_return;
    window = {.platform = sg::window_platform::web_canvas,
              .handle = (void*)"#canvas",
              .client_size = tg::vec2i(preview_width, preview_height)};
#else
    auto created = sr::window_system::try_create({});
    if (created.has_error())
        co_return;
    out.wsys = cc::move(created.value());
    out.win = out.wsys->create_window(
        {.title = cc::string("sg — ray throughput"), .width = preview_width, .height = preview_height});
    window = out.win->native_window();
#endif
    auto chain = ctx->try_create_swapchain({.window = window, .format = format});
    if (chain.has_error())
    {
        cc::println("no display ({}), measuring only", chain.error().to_string());
        co_return;
    }

    auto const building = ctx->cached.acquire_raster_pipeline(rt::blit, {.color = format});
    co_await cc::async_settled(building);
    auto const* const pipeline = building->try_value();
    if (pipeline == nullptr)
    {
        cc::println("the preview's pipeline did not build, measuring only");
        co_return;
    }
    out.swapchain = cc::move(chain.value());
    out.blit = *pipeline;
    out.shown_layout = ctx->cached.acquire_binding_group_layout<rt::shown>();
    co_return;
}

/// Renders the scene's camera view into `preview` and, with a display open, shows it.
/// `camera_ms_per_ray` is what the scene's camera rays measured, negative before they have: the preview is traced
/// only where that says it fits `preview_budget_ms`, and an empty frame is shown otherwise.
/// False once a window asks to close.
[[nodiscard]] cc::shared_async<bool> show_scene(sg::context_handle ctx,
                                                kernels const& k,
                                                display& d,
                                                built_scene const& scene,
                                                sg::texture_2d const& preview,
                                                double camera_ms_per_ray)
{
    if (!d.is_open())
        co_return true;
    if (d.wsys != nullptr)
    {
        d.wsys->poll_events();
        if (d.win->is_close_requested() || d.wsys->is_quit_requested())
            co_return false;
        d.swapchain->set_window_size(tg::vec2i(d.win->width(), d.win->height()));
    }

    auto const preview_rays = double(preview_width) * double(preview_height);
    auto const is_traced = camera_ms_per_ray >= 0.0 && camera_ms_per_ray * preview_rays <= preview_budget_ms;

    auto cmd = ctx->create_command_list();
    if (is_traced)
    {
        auto const group = ctx->transient.create_binding_group(
            *cmd, k.preview_layout,
            rt::preview_scene{.world = scene.tlas->as_view(),
                              .image = preview.as_image_view<sg::pixel_format::rgba8_unorm>()});
        cmd->compute.bind_pipeline(*k.preview);
        cmd->compute.bind_group(0, *group);
        cmd->compute.set_inline_constants(params_for(scene, preview_width, preview_height, 0).to_block());
        cmd->compute.dispatch_threads(preview_width, preview_height, 1);
    }

    auto const target = d.swapchain->acquire_backbuffer();
    {
        auto const shown
            = ctx->transient.create_binding_group(*cmd, d.shown_layout, rt::shown{.texture = preview.as_texture_view()});
        auto pass = cmd->raster.render_to(rt::blit_target{.color = target.cleared(tg::vec4f(0.05f, 0.05f, 0.06f, 1))});
        if (is_traced)
        {
            pass.bind_pipeline(*d.blit);
            pass.bind_group(0, *shown);
            pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
    }
    ctx->submit_command_list_and_present(*d.swapchain, cc::move(cmd));
    ctx->advance_epoch();
    co_await ctx->epochs_in_flight_completion(1);
    co_return true;
}

[[nodiscard]] cc::shared_async<cc::result<sg::context_handle>> create_context()
{
#if RAY_THROUGHPUT_BACKEND_WEBGPU
    auto const requested = co_await cc::async_as_result(sg::request_webgpu_context());
    if (requested.has_error())
        co_return cc::error(cc::any_error(cc::format("no webgpu device: {}", requested.error().underlying().to_string())));
    co_return requested.value();
#elif RAY_THROUGHPUT_BACKEND_DX12
    co_return sg::create_dx12_context({});
#elif RAY_THROUGHPUT_BACKEND_METAL
    co_return sg::create_metal_context({});
#else
    co_return sg::create_vulkan_context({});
#endif
}

/// Settles a compute kernel's pipeline, reporting the compiler's message where it failed.
[[nodiscard]] cc::shared_async<sg::compute_pipeline_handle> settle_kernel(
    cc::shared_async<sg::compute_pipeline_handle> building,
    char const* name)
{
    co_await cc::async_settled(building);
    auto const* const pipeline = building->try_value();
    if (pipeline == nullptr)
    {
        auto const* const error = building->try_error();
        cc::eprintln("kernel {} did not build: {}", name,
                     error != nullptr ? error->underlying().to_string() : cc::string("the build never ran"));
        co_return nullptr;
    }
    co_return *pipeline;
}
} // namespace

ASYNC_EXAMPLE("shaped-graphics/ray-throughput")
{
    auto const created = create_context();
    co_await cc::async_settled(created);
    auto const* const ctx_result = created->try_value();
    if (ctx_result == nullptr || ctx_result->has_error())
    {
        cc::eprintln("no graphics device: {}",
                     ctx_result != nullptr ? ctx_result->error().to_string() : cc::string("the request never ran"));
        co_return;
    }
    auto const ctx = ctx_result->value();
    if (!ctx->supports(sg::feature::ray_query))
    {
        cc::eprintln("this device has no ray queries");
        co_return;
    }
    auto const implementation = ctx->implementation_of(sg::feature::ray_query) == sg::feature_implementation::emulated
                                  ? "the polyfill (emulated)"
                                  : "hardware (native)";

    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    lib.add_package(shaders::package());

    auto k = kernels();
    k.camera = co_await settle_kernel(rt::trace_camera.acquire_pipeline(*ctx), "trace_camera");
    k.probe = co_await settle_kernel(rt::trace_probe.acquire_pipeline(*ctx), "trace_probe");
    k.occlusion = co_await settle_kernel(rt::trace_occlusion.acquire_pipeline(*ctx), "trace_occlusion");
    k.preview = co_await settle_kernel(rt::render_preview.acquire_pipeline(*ctx), "render_preview");
    if (k.camera == nullptr || k.probe == nullptr || k.occlusion == nullptr || k.preview == nullptr)
        co_return;
    k.scene_layout = ctx->cached.acquire_binding_group_layout<rt::scene>();
    k.preview_layout = ctx->cached.acquire_binding_group_layout<rt::preview_scene>();

    auto d = display();
    co_await open_display(ctx, d);

    auto const results = ctx->persistent.create_buffer_from_data(
        cc::vector<u32>::create_defaulted(max_rays_per_dispatch), sg::buffer_usage::readwrite_buffer);
    auto const preview
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = preview_width,
                                             .height = preview_height,
                                             .usage = sg::texture_usage::texture | sg::texture_usage::image});

    cc::println("ray throughput: traced by {}; each batch sized to about {} ms, best of {} batches", implementation,
                target_batch_ms, batches);
    cc::println("| scene | triangles | est. pool MiB | rays | ms / 1M rays | Mrays/s | rays measured | timed by |");
    cc::println("|---|---:|---:|---|---:|---:|---:|---|");

    // Built one at a time, so only one scene holds the polyfill's pool at once.
    // The soups go last: they are the slowest by far, and the rows that matter arrive before them.
    using builder = built_scene (*)(sg::context&);
    builder const builders[] = {
        [](sg::context& c) { return build_heightfield_scene(c, 363, triangle_order::coherent); },
        [](sg::context& c) { return build_heightfield_scene(c, 363, triangle_order::morton_sorted); },
        [](sg::context& c) { return build_assembly_scene(c, 24, 4); },
        [](sg::context& c) { return build_heightfield_scene(c, 725, triangle_order::coherent); },
        [](sg::context& c) { return build_heightfield_scene(c, 725, triangle_order::morton_sorted); },
        [](sg::context& c) { return build_heightfield_scene(c, 363, triangle_order::shuffled); },
        [](sg::context& c) { return build_heightfield_scene(c, 725, triangle_order::shuffled); },
    };
    constexpr isize scene_count = isize(sizeof(builders) / sizeof(builders[0]));
    ray_kind const kinds[] = {ray_kind::camera, ray_kind::probe, ray_kind::occlusion};
    // What each scene's camera rays measured, which decides whether its preview may be traced.
    auto camera_cost = cc::vector<double>::create_filled(scene_count, -1.0);

    auto keep_going = true;
    for (auto at = isize(0); at < scene_count; ++at)
    {
        auto const build = builders[at];
        if (!keep_going)
            break;
        // The last scene's pool region is free only once its epoch retires, and two large scenes do not fit at once.
        ctx->advance_epoch();
        co_await ctx->idle_completion();

        auto scene = built_scene();
        auto build_error = cc::string();
        try
        {
            scene = build(*ctx);
        }
        catch (sg::exception const& e)
        {
            build_error = cc::string(e.message());
        }
        if (!build_error.empty())
        {
            cc::println("| a scene did not build: {} |", build_error);
            continue;
        }
        keep_going = co_await show_scene(ctx, k, d, scene, preview, -1.0);

        for (auto const kind : kinds)
        {
            // Small dispatches price a ray, growing only while they stay fast; the best of `batches` is cut to that price.
            // A scene too slow for this device to trace even 256 rays quickly is skipped, not risked.
            auto trial_shape = batch_shape();
            auto trial = co_await time_batch(ctx, k, scene, results, kind, trial_shape, 1);
            auto trial_ms = trial.gpu_ms >= 0.0 ? trial.gpu_ms : trial.wall_ms;
            if (trial_ms > slowest_trial_ms)
            {
                cc::println("| {} | {} | {:.1f} | {} | skipped: {} rays took {:.0f} ms, too slow to measure here |",
                            scene.name, scene.triangles, scene.estimated_pool_mib, name_of(kind),
                            trial_shape.rays_per_dispatch(), trial_ms);
                continue;
            }
            while (trial_shape.width < 128 && trial_ms < 4.0)
            {
                trial_shape.width *= 2;
                trial = co_await time_batch(ctx, k, scene, results, kind, trial_shape, 1);
                trial_ms = trial.gpu_ms >= 0.0 ? trial.gpu_ms : trial.wall_ms;
            }
            auto const shape = shape_for(trial_ms / trial_shape.rays());
            if (kind == ray_kind::camera)
                camera_cost[at] = trial_ms / trial_shape.rays();
            auto best = batch_time{.gpu_ms = 1e30, .wall_ms = 1e30};
            for (auto b = 0; b < batches; ++b)
            {
                auto const t
                    = co_await time_batch(ctx, k, scene, results, kind, shape, u32(100 + b * max_dispatches_per_batch));
                if (t.gpu_ms >= 0.0)
                    best.gpu_ms = cc::min(best.gpu_ms, t.gpu_ms);
                best.wall_ms = cc::min(best.wall_ms, t.wall_ms);
                keep_going = keep_going && co_await show_scene(ctx, k, d, scene, preview, camera_cost[at]);
            }
            auto const is_gpu_timed = best.gpu_ms < 1e29;
            auto const ms = is_gpu_timed ? best.gpu_ms : best.wall_ms;
            auto const rays = shape.rays();
            auto const ms_per_million = ms / (rays / 1e6);
            cc::println("| {} | {} | {:.1f} | {} | {:.3f} | {:.2f} | {} | {} |", scene.name, scene.triangles,
                        scene.estimated_pool_mib, name_of(kind), ms_per_million, rays / (ms * 1000.0), i64(rays),
                        is_gpu_timed ? "gpu" : "wall clock");
        }
        scene = built_scene();
    }
    cc::println("done");

    // The first three scenes stay on show until the window closes, or for as long as the page is open; one each two
    // seconds, each traced only where its measured cost allows.
    // The large ones are left out, since together they would outgrow the polyfill's pool, and the soups since tracing
    // one is the very thing that resets a slow GPU.
    // The stripes are triangle parity, so the coherent field shows rows and the sorted one shows its Morton blocks.
    // A display that never opened, under node or without a window backend, ends here.
    if (d.is_open())
    {
        auto shown = cc::vector<built_scene>();
        for (auto i = 0; i < 3; ++i)
            shown.push_back(builders[i](*ctx));
        auto at = isize(0);
        auto since = cc::current_time_steady_secs();
        while (keep_going)
        {
            keep_going = co_await show_scene(ctx, k, d, shown[at], preview, camera_cost[at]);
            if (cc::current_time_steady_secs() - since > 2.0)
            {
                at = (at + 1) % shown.size();
                since = cc::current_time_steady_secs();
            }
        }
    }

    ctx->advance_epoch();
    co_await ctx->idle_completion();
    ctx->shutdown();
}
