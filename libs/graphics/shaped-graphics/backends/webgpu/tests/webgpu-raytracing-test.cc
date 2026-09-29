#include "webgpu-test-common.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// The ray-query polyfill's pool as the builds leave it, read back and checked against
// libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md, and the group-3 bindings a traced shader reads it through.

namespace
{
namespace webgpu = sg::backend::webgpu;
using webgpu::test::make_shader;

struct vec3
{
    float x = 0, y = 0, z = 0;
};

struct box
{
    vec3 lo;
    vec3 hi;
};

/// The pool as downloaded, read in units.
struct pool_reader
{
    cc::span<byte const> bytes;

    [[nodiscard]] u32 word(u32 unit, int k) const
    {
        auto v = u32(0);
        cc::memcpy(&v, bytes.data() + isize(unit) * 16 + k * 4, sizeof(v));
        return v;
    }
    [[nodiscard]] float real(u32 unit, int k) const { return cc::bit_cast<float>(word(unit, k)); }
    [[nodiscard]] vec3 xyz(u32 unit) const { return {real(unit, 0), real(unit, 1), real(unit, 2)}; }

    [[nodiscard]] u32 node_unit(u32 region, u32 node) const { return region + 1 + 2 * node; }
    [[nodiscard]] box node_box(u32 region, u32 node) const
    {
        return {xyz(node_unit(region, node)), xyz(node_unit(region, node) + 1)};
    }
};

[[nodiscard]] float magnitude(float f)
{
    return f < 0 ? -f : f;
}

[[nodiscard]] bool contains(box const& outer, box const& inner, float eps = 0)
{
    return outer.lo.x - eps <= inner.lo.x && outer.lo.y - eps <= inner.lo.y && outer.lo.z - eps <= inner.lo.z
        && inner.hi.x <= outer.hi.x + eps && inner.hi.y <= outer.hi.y + eps && inner.hi.z <= outer.hi.z + eps;
}

[[nodiscard]] box bounds_of(cc::span<vec3 const> points)
{
    auto b = box{.lo = points[0], .hi = points[0]};
    for (auto const& p : points)
    {
        b.lo = {cc::min(b.lo.x, p.x), cc::min(b.lo.y, p.y), cc::min(b.lo.z, p.z)};
        b.hi = {cc::max(b.hi.x, p.x), cc::max(b.hi.y, p.y), cc::max(b.hi.z, p.z)};
    }
    return b;
}

/// What walking a region's tree from its root found.
struct walk_result
{
    int nodes_visited = 0;
    bool boxes_nest = true;
    bool leaves_hold_at_most_four = true;
    cc::vector<int> hits; // per primitive, how often a leaf named it
};

/// Walks the region's tree; `primitive_box` is each primitive's own box, which its leaf must hold.
template <class PrimitiveBox>
[[nodiscard]] walk_result walk(pool_reader const& pool, u32 region, PrimitiveBox&& primitive_box, float eps = 0)
{
    auto result = walk_result{};
    auto const node_count = pool.word(region, 0);
    result.hits = cc::vector<int>::create_filled(isize(pool.word(region, 2)), 0);
    auto stack = cc::vector<u32>();
    stack.push_back(0);
    while (!stack.empty())
    {
        auto const node = stack.pop_back();
        ++result.nodes_visited;
        if (node >= node_count || result.nodes_visited > int(node_count))
        {
            result.boxes_nest = false;
            break;
        }
        auto const outer = pool.node_box(region, node);
        auto const w0 = pool.word(pool.node_unit(region, node), 3);
        auto const w1 = pool.word(pool.node_unit(region, node) + 1, 3);
        if ((w1 & 0x80000000u) != 0)
        {
            auto const count = w1 & 0x7fffffffu;
            result.leaves_hold_at_most_four = result.leaves_hold_at_most_four && count >= 1 && count <= 4;
            for (auto j = w0; j < w0 + count && j < u32(result.hits.size()); ++j)
            {
                ++result.hits[j];
                result.boxes_nest = result.boxes_nest && contains(outer, primitive_box(j), eps);
            }
        }
        else
        {
            for (auto const child : {w0, w1})
            {
                result.boxes_nest = result.boxes_nest && contains(outer, pool.node_box(region, child));
                stack.push_back(child);
            }
        }
    }
    return result;
}

[[nodiscard]] bool each_exactly_once(cc::span<int const> hits)
{
    for (auto const h : hits)
        if (h != 1)
            return false;
    return true;
}

/// `m` applied to `p`, both as tlas_instance::transform lays a 3x4 out.
[[nodiscard]] vec3 apply(float const* m, vec3 p)
{
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

[[nodiscard]] sg::raw_buffer_handle build_input(sg::context& ctx, cc::span<float const> data)
{
    return ctx.persistent.create_buffer_from_data(data, sg::buffer_usage::accel_structure_build_input).raw();
}

[[nodiscard]] sg::raw_buffer_handle build_input(sg::context& ctx, cc::span<u32 const> data)
{
    return ctx.persistent.create_buffer_from_data(data, sg::buffer_usage::accel_structure_build_input).raw();
}

[[nodiscard]] sg::raw_buffer_handle build_input(sg::context& ctx, cc::span<u16 const> data)
{
    return ctx.persistent.create_buffer_from_data(data, sg::buffer_usage::accel_structure_build_input).raw();
}

/// One triangle the build must have written: its object-space vertices and its three `w` words.
struct expected_triangle
{
    vec3 v[3];
    u32 primitive = 0;
    u32 geometry = 0;
    u32 opaque = 0;
};

[[nodiscard]] vec3 geometry1_vertex(int k)
{
    return {float(k), float(k * k), -float(k)};
}

[[nodiscard]] vec3 geometry2_vertex(int k)
{
    return {float(k) + 0.5f, 3.0f, float(k % 2)};
}

constexpr char const* k_read_roots = R"(
struct constants_data {
    value: u32,
    unused: u32,
}
@group(3) @binding(0) var<uniform> constants: constants_data;
@group(3) @binding(17) var<storage, read> sg_acceleration_pool: array<vec4u>;
@group(3) @binding(18) var<uniform> sg_acceleration_roots: array<vec4u, 4>;
@group(0) @binding(1) var<storage, read_write> Output: array<u32>;

@compute @workgroup_size(1)
fn main() {
    let scene = sg_acceleration_roots[0].y;
    Output[0] = sg_acceleration_roots[0].x;
    Output[1] = scene;
    Output[2] = sg_acceleration_pool[scene].z;
    Output[3] = constants.value;
    Output[4] = sg_acceleration_pool[0].x;
}
)";
} // namespace

ASYNC_INVOCABLE_TEST("sg webgpu - builds lay the pool out as the ray-query polyfill reads it",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    REQUIRE(ctx.supports(sg::feature::ray_query));
    CHECK(ctx.implementation_of(sg::feature::ray_query) == sg::feature_implementation::emulated);
    CHECK(!ctx.supports(sg::feature::raytracing_pipeline));

    // Geometry 0: two unindexed triangles under a per-geometry transform.
    float const g0_vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 1, 0, 1, 2, 1};
    float const g0_transform[] = {2, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};

    // Geometry 1: four triangles through 16-bit indices starting at an odd index, into interleaved vertices past a skipped record.
    auto g1_vertices = cc::vector<float>();
    for (auto k = -1; k < 5; ++k)
    {
        auto const p = geometry1_vertex(k);
        for (auto const f : {p.x, p.y, p.z, 7.0f, 7.0f, 7.0f})
            g1_vertices.push_back(f);
    }
    u16 const g1_indices[] = {999, 0, 1, 2, 2, 1, 3, 3, 4, 2, 4, 0, 1, 999};

    // Geometry 2: three triangles through 32-bit indices, not opaque.
    auto g2_vertices = cc::vector<float>();
    for (auto k = 0; k < 6; ++k)
    {
        auto const p = geometry2_vertex(k);
        for (auto const f : {p.x, p.y, p.z})
            g2_vertices.push_back(f);
    }
    u32 const g2_indices[] = {12345, 0, 1, 2, 3, 4, 5, 5, 0, 3};

    sg::blas_triangles const triangles[] = {
        {.vertices = build_input(ctx, g0_vertices), .vertex_count = 6, .transform = build_input(ctx, g0_transform)},
        {.vertices = build_input(ctx, g1_vertices),
         .vertex_count = 5,
         .vertex_stride_in_bytes = 24,
         .vertex_offset_in_bytes = 24,
         .indices = build_input(ctx, g1_indices),
         .index_count = 12,
         .index_offset_in_bytes = 2,
         .index_type = sg::index_format::uint16},
        {.vertices = build_input(ctx, g2_vertices),
         .vertex_count = 6,
         .indices = build_input(ctx, g2_indices),
         .index_count = 9,
         .index_offset_in_bytes = 4,
         .index_type = sg::index_format::uint32,
         .is_opaque = false},
    };

    auto expected = cc::vector<expected_triangle>();
    for (u32 t = 0; t < 2; ++t)
    {
        auto tri = expected_triangle{.primitive = t, .geometry = 0, .opaque = 1};
        for (auto k = 0; k < 3; ++k)
        {
            auto const* v = g0_vertices + (3 * t + k) * 3;
            tri.v[k] = apply(g0_transform, {v[0], v[1], v[2]});
        }
        expected.push_back(tri);
    }
    for (u32 t = 0; t < 4; ++t)
    {
        auto tri = expected_triangle{.primitive = t, .geometry = 1, .opaque = 1};
        for (auto k = 0; k < 3; ++k)
            tri.v[k] = geometry1_vertex(int(g1_indices[1 + 3 * t + k]));
        expected.push_back(tri);
    }
    for (u32 t = 0; t < 3; ++t)
    {
        auto tri = expected_triangle{.primitive = t, .geometry = 2, .opaque = 0};
        for (auto k = 0; k < 3; ++k)
            tri.v[k] = geometry2_vertex(int(g2_indices[1 + 3 * t + k]));
        expected.push_back(tri);
    }

    // Boxes: five in 8-float records, not opaque, then two in 6-float ones.
    auto g3_boxes = cc::vector<float>();
    for (auto k = 0; k < 5; ++k)
        for (auto const f : {float(k), 0.0f, -1.0f, float(k) + 0.5f, 1.0f, float(k), 9.0f, 9.0f})
            g3_boxes.push_back(f);
    float const g4_boxes[] = {-3, -3, -3, -2, -2, -2, 4, 4, 4, 6, 7, 8};
    sg::blas_aabbs const boxes[] = {
        {.aabbs = build_input(ctx, g3_boxes), .aabb_count = 5, .aabb_stride_in_bytes = 32, .is_opaque = false},
        {.aabbs = build_input(ctx, g4_boxes), .aabb_count = 2},
    };

    auto cmd = ctx.create_command_list();
    auto const triangle_blas = cmd->raytracing.build_blas(triangles);
    auto const box_blas = cmd->raytracing.build_blas(boxes);

    sg::tlas_instance const instances[] = {
        {.blas = triangle_blas,
         .transform = {0, -2, 0, 1, 2, 0, 0, 2, 0, 0, 2, 3},
         .instance_id = 5,
         .hit_group_offset = 3,
         .mask = 0x0f,
         .cull_mode = sg::instance_cull_mode::front,
         .opaque_override = true},
        {.blas = box_blas,
         .transform = {1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0},
         .instance_id = (1u << 24) - 1,
         .hit_group_offset = 0,
         .mask = 0xff,
         .cull_mode = sg::instance_cull_mode::none,
         .opaque_override = false},
    };
    auto const tlas = cmd->raytracing.build_tlas(instances);
    auto const future = static_cast<webgpu::webgpu_command_list&>(*cmd).download_acceleration_pool();
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    auto const pool = pool_reader{.bytes = bytes.span()};
    REQUIRE(bytes.size() == ctx._acceleration.capacity_units() * webgpu::acceleration_unit_bytes);

    // Unit 0 is the empty TLAS.
    CHECK(pool.word(0, 0) == 0u);

    // The triangle BLAS: 9 triangles, 3 leaves, 5 nodes.
    {
        auto const& blas = static_cast<webgpu::webgpu_blas const&>(*triangle_blas);
        auto const region = blas.unit();
        CHECK(pool.word(region, 0) == 5u);
        CHECK(pool.word(region, 1) == region + 1 + 2 * 5);
        CHECK(pool.word(region, 2) == 9u);
        CHECK(pool.word(region, 3) == 0u);
        CHECK(blas.units() == 1 + 2 * 5 + 3 * 9);

        auto const first = pool.word(region, 1);
        auto mismatches = 0;
        for (u32 j = 0; j < 9; ++j)
        {
            auto const& e = expected[j];
            for (auto k = 0; k < 3; ++k)
            {
                auto const v = pool.xyz(first + 3 * j + u32(k));
                if (v.x != e.v[k].x || v.y != e.v[k].y || v.z != e.v[k].z)
                    ++mismatches;
            }
            if (pool.word(first + 3 * j, 3) != e.primitive || pool.word(first + 3 * j + 1, 3) != e.geometry
                || pool.word(first + 3 * j + 2, 3) != e.opaque)
                ++mismatches;
        }
        CHECK(mismatches == 0);

        auto const walked = walk(pool, region,
                                 [&](u32 j)
                                 {
                                     vec3 const v[] = {pool.xyz(first + 3 * j), pool.xyz(first + 3 * j + 1),
                                                       pool.xyz(first + 3 * j + 2)};
                                     return bounds_of(v);
                                 });
        CHECK(walked.nodes_visited == 5);
        CHECK(walked.boxes_nest);
        CHECK(walked.leaves_hold_at_most_four);
        CHECK(each_exactly_once(walked.hits));
    }

    // The box BLAS: 7 boxes, 2 leaves, 3 nodes.
    {
        auto const& blas = static_cast<webgpu::webgpu_blas const&>(*box_blas);
        auto const region = blas.unit();
        CHECK(pool.word(region, 0) == 3u);
        CHECK(pool.word(region, 2) == 7u);
        CHECK(pool.word(region, 3) == 1u);

        auto const first = pool.word(region, 1);
        auto mismatches = 0;
        for (u32 j = 0; j < 7; ++j)
        {
            auto const* source = j < 5 ? g3_boxes.data() + 8 * j : g4_boxes + 6 * (j - 5);
            auto const lo = pool.xyz(first + 2 * j);
            auto const hi = pool.xyz(first + 2 * j + 1);
            if (lo.x != source[0] || lo.y != source[1] || lo.z != source[2] || hi.x != source[3] || hi.y != source[4]
                || hi.z != source[5])
                ++mismatches;
            auto const geometry = j < 5 ? 0u : 1u;
            auto const primitive = j < 5 ? j : j - 5;
            auto const opaque_bit = j < 5 ? 0u : 0x80000000u;
            if (pool.word(first + 2 * j, 3) != primitive || pool.word(first + 2 * j + 1, 3) != (geometry | opaque_bit))
                ++mismatches;
        }
        CHECK(mismatches == 0);

        auto const walked
            = walk(pool, region, [&](u32 j) { return box{pool.xyz(first + 2 * j), pool.xyz(first + 2 * j + 1)}; });
        CHECK(walked.nodes_visited == 3);
        CHECK(walked.boxes_nest);
        CHECK(each_exactly_once(walked.hits));
    }

    // The TLAS: 2 instances in 1 leaf, both kinds present.
    {
        auto const& t = static_cast<webgpu::webgpu_tlas const&>(*tlas);
        auto const region = t.unit();
        CHECK(pool.word(region, 0) == 1u);
        CHECK(pool.word(region, 1) == region + 3);
        CHECK(pool.word(region, 2) == 2u);
        CHECK(pool.word(region, 3) == 3u);

        auto const first = pool.word(region, 1);
        u32 const expected_flags[] = {1u | (1u << 2), 2u | (2u << 2)};
        auto mismatches = 0;
        auto worst_identity_error = 0.0f;
        for (u32 i = 0; i < 2; ++i)
        {
            auto const& inst = instances[i];
            auto const record = first + 7 * i;
            float world_to_object[12];
            float object_to_world[12];
            for (auto k = 0; k < 12; ++k)
            {
                world_to_object[k] = pool.real(record + u32(k / 4), k % 4);
                object_to_world[k] = pool.real(record + 3 + u32(k / 4), k % 4);
                if (object_to_world[k] != inst.transform[k])
                    ++mismatches;
            }

            // world_to_object after object_to_world is the identity, on the basis vectors and the origin alike.
            for (auto const p : {vec3{0, 0, 0}, vec3{1, 0, 0}, vec3{0, 1, 0}, vec3{0, 0, 1}})
            {
                auto const back = apply(world_to_object, apply(object_to_world, p));
                worst_identity_error = cc::max(
                    worst_identity_error,
                    cc::max(magnitude(back.x - p.x), cc::max(magnitude(back.y - p.y), magnitude(back.z - p.z))));
            }

            auto const& blas = static_cast<webgpu::webgpu_blas const&>(*inst.blas);
            if (pool.word(record + 6, 0) != blas.unit()
                || pool.word(record + 6, 1) != (inst.instance_id | (u32(inst.mask) << 24))
                || pool.word(record + 6, 2) != (inst.hit_group_offset | (expected_flags[i] << 24))
                || pool.word(record + 6, 3) != i)
                ++mismatches;
        }
        CHECK(mismatches == 0);
        CHECK(worst_identity_error < 1e-5f);

        auto const walked = walk(
            pool, region,
            [&](u32 i)
            {
                auto const& blas = static_cast<webgpu::webgpu_blas const&>(*instances[i].blas);
                auto const root = pool.node_box(blas.unit(), 0);
                auto corners = cc::vector<vec3>();
                for (auto c = 0; c < 8; ++c)
                    corners.push_back(apply(instances[i].transform,
                                            {(c & 1) != 0 ? root.hi.x : root.lo.x, (c & 2) != 0 ? root.hi.y : root.lo.y,
                                             (c & 4) != 0 ? root.hi.z : root.lo.z}));
                return bounds_of(corners);
            },
            1e-4f);
        CHECK(walked.nodes_visited == 1);
        CHECK(walked.boxes_nest);
        CHECK(each_exactly_once(walked.hits));
    }
}

ASYNC_INVOCABLE_TEST("sg webgpu - a grown pool keeps every region, the ones an open list wrote included",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    float const small_vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    sg::blas_triangles const small = {.vertices = build_input(ctx, small_vertices), .vertex_count = 3};

    auto cmd = ctx.create_command_list();
    auto const small_blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&small, 1));
    auto const before = ctx._acceleration.generation();

    // More triangles than the pool has units at all, so building them must grow it while the small one is unsubmitted.
    auto const big_count = int(ctx._acceleration.capacity_units() / 3) + 100;
    auto big_vertices = cc::vector<float>();
    for (auto t = 0; t < big_count; ++t)
        for (auto const f : {float(t), 0.0f, 0.0f, float(t) + 1, 0.0f, 0.0f, float(t), 1.0f, 0.0f})
            big_vertices.push_back(f);
    sg::blas_triangles const big = {.vertices = build_input(ctx, big_vertices), .vertex_count = 3 * big_count};
    auto const big_blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&big, 1));
    CHECK(ctx._acceleration.generation() > before);

    // The TLAS reads the small BLAS's root box, which this list wrote into the outgrown buffer.
    sg::tlas_instance const instances[] = {{.blas = small_blas}, {.blas = big_blas}};
    auto const tlas = cmd->raytracing.build_tlas(instances);
    auto const future = static_cast<webgpu::webgpu_command_list&>(*cmd).download_acceleration_pool();
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    auto const pool = pool_reader{.bytes = bytes.span()};

    auto const small_region = static_cast<webgpu::webgpu_blas const&>(*small_blas).unit();
    auto const first = pool.word(small_region, 1);
    CHECK(pool.word(small_region, 2) == 1u);
    CHECK(pool.xyz(first + 1).x == 1.0f);
    CHECK(pool.xyz(first + 2).y == 1.0f);

    auto const root = pool.node_box(static_cast<webgpu::webgpu_tlas const&>(*tlas).unit(), 0);
    CHECK(contains(root, box{{0, 0, 0}, {float(big_count), 1, 0}}));
}

ASYNC_INVOCABLE_TEST("sg webgpu - a dispatch reads its acceleration roots from group 3, in binding order",
                     (webgpu::webgpu_context_handle const& handle))
{
    auto& ctx = *handle;
    float const vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    sg::blas_triangles const tri = {.vertices = build_input(ctx, vertices), .vertex_count = 3};

    auto build = ctx.create_command_list();
    auto const blas = build->raytracing.build_blas(cc::span<sg::blas_triangles const>(&tri, 1));
    sg::tlas_instance const instances[] = {{.blas = blas}, {.blas = blas}, {.blas = blas}};
    auto const tlas = build->raytracing.build_tlas(instances);
    ctx.submit_command_list(cc::move(build));

    // Group 0 holds a null structure at binding 2, group 1 the scene: the roots count group 0's first.
    auto const output_binding = sg::binding{.name = "Output",
                                            .group_index = 0,
                                            .index = 1,
                                            .type = sg::binding_type::buffer,
                                            .access = sg::access_mode::read_write};
    auto const nothing_binding
        = sg::binding{.name = "nothing", .group_index = 0, .index = 2, .type = sg::binding_type::acceleration_structure};
    auto const scene_binding
        = sg::binding{.name = "scene", .group_index = 1, .index = 0, .type = sg::binding_type::acceleration_structure};
    auto const shader = make_shader(sg::shader_stage::compute, k_read_roots, "main",
                                    {output_binding, nothing_binding, scene_binding}, sg::compute_dimensions{});

    auto group0_bindings = cc::vector<sg::binding>{shader.bindings[0], shader.bindings[1]};
    auto group1_bindings = cc::vector<sg::binding>{shader.bindings[2]};
    auto group0_layout = ctx.uncached.create_binding_group_layout(group0_bindings);
    auto group1_layout = ctx.uncached.create_binding_group_layout(group1_bindings);
    auto pipeline_layout = ctx.uncached.create_pipeline_layout({
        .groups = {group0_layout, group1_layout},
        .inline_constants
        = sg::binding{.name = "constants", .index = 0, .type = sg::binding_type::constants_buffer, .block_size = 8},
    });
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    auto output
        = ctx.persistent.create_raw_buffer(5 * 4, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    sg::named_view const group0_views[] = {
        {.name = "Output", .view = sg::buffer<u32>::from_raw(output).as_readwrite_buffer()},
        {.name = "nothing", .view = sg::tlas_view{}},
    };
    sg::named_view const group1_views[] = {{.name = "scene", .view = tlas->as_view()}};
    auto group0 = ctx.persistent.create_binding_group(group0_layout, group0_views);
    auto group1 = ctx.persistent.create_binding_group(group1_layout, group1_views);

    struct constants
    {
        u32 value;
        u32 unused;
    };

    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group0);
    cmd->compute.bind_group(1, *group1);
    cmd->compute.set_inline_constants(constants{.value = 77, .unused = 0});
    cmd->compute.dispatch_groups(1, 1, 1);
    auto const future = cmd->download.data_from_buffer<u32>(output, 0, 5);
    ctx.submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == 5);
    CHECK(data[0] == 0u);
    CHECK(data[1] == static_cast<webgpu::webgpu_tlas const&>(*tlas).unit());
    CHECK(data[2] == 3u);
    CHECK(data[3] == 77u);
    CHECK(data[4] == 0u);
}
