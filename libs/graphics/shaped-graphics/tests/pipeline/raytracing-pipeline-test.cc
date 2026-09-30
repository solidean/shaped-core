#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// raytracing_pipeline.sgl's `path` against ray-query-test.cc's scene: the same cells, hit through a pipeline's tables.
//
// The scene: one BLAS of two triangles over the square [0, 4]², non-opaque, so the any hits are asked.
// Triangle 0 is the half below the diagonal x + y = 4, triangle 1 the half above it, and every any hit cuts it out.
// Instance 0 (id 10) stands at z = 0, instance 1 (id 20) is moved by (4, 0, 2).
// A surface hit traces a shadow ray by (4, 0, 2), which from instance 0 lands on instance 1 and from instance 1 on
// nothing: so instance 0 is in shadow and instance 1 is lit.

namespace
{
constexpr int grid = 8;

using path_t = shaders::raytracing_pipeline_path_t;

struct expected_cell
{
    bool is_hit = false;
    int instance_id = 0;
    int primitive = 0;
    bool is_lit = false;
    float t = 0;
    float u = 0;
    float v = 0;
};

expected_cell expected_at(int x, int y)
{
    auto const px = float(x) + 0.25f;
    auto const py = float(y) + 0.5f;
    if (py >= 4)
        return {};
    auto const instance = px < 4 ? 0 : 1;
    auto const lx = instance == 0 ? px : px - 4;
    auto const primitive = lx + py < 4 ? 0 : 1;
    if (primitive == 1)
        return {};
    return {.is_hit = true,
            .instance_id = instance == 0 ? 10 : 20,
            .primitive = 0,
            .is_lit = instance == 1,
            .t = instance == 0 ? 1.0f : 3.0f,
            .u = lx / 4,
            .v = py / 4};
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL ray-tracing pipeline traces two ray types, one from the other's closest hit",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::raytracing_pipeline))
        SKIP("this device has no ray-tracing pipelines");

    auto const desc = co_await shaders::raytracing_pipeline.path.description(*ctx);
    CHECK(desc.max_recursion_depth == 2u);
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    auto table_desc = path_t::table_description(pipeline);
    auto const row = path_t::add_row(table_desc, path_t::hit_groups_t::textured);
    auto const table = ctx->uncached.create_raytracing_shader_table(table_desc);
    REQUIRE(table != nullptr);

    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    auto const input = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const triangles = sg::blas_triangles{.vertices = input.raw(), .vertex_count = 6, .is_opaque = false};
    // a record per ray type, which metal bakes into the BLAS
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&triangles, 1),
                                                 sg::accel_build_flag::fast_trace, path_t::ray_count);
    auto const offset = table->offset_of(row);
    sg::tlas_instance const instances[] = {
        {.blas = blas, .instance_id = 10, .hit_group_offset = offset},
        {.blas = blas, .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 2}, .instance_id = 20, .hit_group_offset = offset},
    };
    auto const tlas = cmd->raytracing.build_tlas(instances);

    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::traced>();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::traced{.world = tlas->as_view(), .hits = hits.as_readwrite_buffer(), .ids = ids.as_readwrite_buffer()});
    cmd->raytracing.bind_pipeline(*pipeline);
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), grid, grid);
    auto const hits_back = cmd->download.data_from_buffer(hits);
    auto const ids_back = cmd->download.data_from_buffer(ids);
    ctx->submit_command_list(cc::move(cmd));

    auto const got_hits = co_await hits_back.data();
    auto const got_ids = co_await ids_back.data();
    REQUIRE(got_hits.size() == grid * grid);
    REQUIRE(got_ids.size() == grid * grid);
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const want = expected_at(x, y);
            auto const at = y * grid + x;
            auto const where = cc::format("cell ({}, {})", x, y);
            CHECK(got_ids[at][0] == (want.is_hit ? 1 : 0)).context(where);
            if (!want.is_hit)
                continue;
            CHECK(got_ids[at][1] == want.instance_id).context(where);
            CHECK(got_ids[at][2] == want.primitive).context(where);
            CHECK(got_ids[at][3] == (want.is_lit ? 1 : 0)).context(where);
            CHECK(tg::abs(got_hits[at][0] - want.t) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][1] - want.u) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][2] - want.v) < 1e-4f).context(where);
        }
}

// The same pipeline's procedural group: one instance, id 30, of a BLAS of two boxes, each holding the unit sphere at its
// centre, and the primary rays of the grid above.
// The intersection reports each sphere's normal, which the closest hit writes where a triangle's barycentrics stand.

ASYNC_INVOCABLE_TEST("sg - an SGL ray-tracing pipeline's procedural group reports through its intersection",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::raytracing_pipeline))
        SKIP("this device has no ray-tracing pipelines");

    auto const desc = co_await shaders::raytracing_pipeline.path.description(*ctx);
    CHECK(desc.max_attribute_size == 12);
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    auto table_desc = path_t::table_description(pipeline);
    (void)path_t::add_row(table_desc, path_t::hit_groups_t::textured);
    auto const row = path_t::add_row(table_desc, path_t::hit_groups_t::spheres);
    auto const table = ctx->uncached.create_raytracing_shader_table(table_desc);
    REQUIRE(table != nullptr);

    float const boxes[] = {0, 0, 0, 2, 2, 2, 2, 0, 0, 4, 2, 2};
    auto const input = ctx->persistent.create_buffer_from_data(boxes, sg::buffer_usage::accel_structure_build_input);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const aabbs = sg::blas_aabbs{.aabbs = input.raw(), .aabb_count = 2};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&aabbs, 1),
                                                 sg::accel_build_flag::fast_trace, path_t::ray_count);
    sg::tlas_instance const instances[] = {{.blas = blas, .instance_id = 30, .hit_group_offset = table->offset_of(row)}};
    auto const tlas = cmd->raytracing.build_tlas(instances);

    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::traced>();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::traced{.world = tlas->as_view(), .hits = hits.as_readwrite_buffer(), .ids = ids.as_readwrite_buffer()});
    cmd->raytracing.bind_pipeline(*pipeline);
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), grid, grid);
    auto const hits_back = cmd->download.data_from_buffer(hits);
    auto const ids_back = cmd->download.data_from_buffer(ids);
    ctx->submit_command_list(cc::move(cmd));

    auto const got_hits = co_await hits_back.data();
    auto const got_ids = co_await ids_back.data();
    REQUIRE(got_hits.size() == grid * grid);
    REQUIRE(got_ids.size() == grid * grid);
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const px = float(x) + 0.25f;
            auto const py = float(y) + 0.5f;
            auto const primitive = px < 2 ? 0 : 1;
            auto const dx = px - float(2 * primitive + 1);
            auto const dy = py - 1;
            auto const is_hit = px < 4 && dx * dx + dy * dy < 1;
            auto const at = y * grid + x;
            auto const where = cc::format("cell ({}, {})", x, y);
            CHECK(got_ids[at][0] == (is_hit ? 1 : 0)).context(where);
            if (!is_hit)
                continue;
            CHECK(got_ids[at][1] == 30).context(where);
            CHECK(got_ids[at][2] == primitive).context(where);
            CHECK(tg::abs(got_hits[at][0] - (2 - tg::sqrt(1 - dx * dx - dy * dy))) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][1] - dx) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][2] - dy) < 1e-4f).context(where);
        }
}

// `open_path` leaves every hit group to the host, which compiles one at run time from SGL it wrote: `textured`, again.
// It traces the triangle grid of the first test, so a cell sees what that test's cells see.

namespace
{
constexpr auto k_runtime_group = R"(require raytracing_pipeline

binding traced_open:
    world: acceleration_structure[.triangles]
    hits: mut buffer[float4]
    ids: mut buffer[int4]

struct radiance:
    t: float
    u: float
    v: float
    instance_id: int
    primitive: int
    is_hit: int
    is_lit: int

struct shadow:
    is_lit: bool

rays path_rays:
    surface: radiance
    occlusion: shadow

@closest_hit fun shade(h: triangle_hit, p: mut radiance){traced_open}:
    let mut s = shadow(false)
    let o = h.ray.origin + h.ray.direction * h.t
    let r = ray(origin = o, direction = vec3(4.0, 0.0, 2.0), t_min = 0.001, t_max = 2.0)
    trace(traced_open.world, r, path_rays.occlusion, mut s, flags = ray_flags.accept_first_hit_and_end_search | ray_flags.skip_closest_hit)
    p.t = h.t
    p.u = h.barycentrics.y
    p.v = h.barycentrics.z
    p.instance_id = h.instance_id
    p.primitive = h.primitive_index
    p.is_hit = 1
    if s.is_lit => p.is_lit = 1

@any_hit fun cutout(c: triangle_candidate, p: mut radiance) -> hit_decision:
    if c.primitive_index == 1 => return hit_decision.ignore
    return hit_decision.accept

@any_hit fun shadow_cutout(c: triangle_candidate, s: mut shadow) -> hit_decision:
    if c.primitive_index == 1 => return hit_decision.ignore
    return hit_decision.accept

hit_group material for path_rays:
    surface = (closest_hit = shade, any_hit = cutout)
    occlusion = (any_hit = shadow_cutout)
)";
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL ray-tracing pipeline takes a hit group the host compiled at run time",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::raytracing_pipeline))
        SKIP("this device has no ray-tracing pipelines");

    using open_t = shaders::raytracing_open_open_path_t;
    auto const hit_shaders = co_await slib::compile_hit_group(
        ctx.get(), &sg_test::shader_fixtures(), &open_t::definition(), k_runtime_group, "material", "runtime.sgl");
    REQUIRE(hit_shaders.size() == open_t::ray_count);
    auto const desc = co_await shaders::raytracing_open.open_path.description(*ctx, {.hit_groups = hit_shaders});
    CHECK(desc.max_recursion_depth == 2u);
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    auto table_desc = open_t::table_description(pipeline);
    auto const row = open_t::add_row(table_desc, open_t::first_host_hit_group);
    auto const table = ctx->uncached.create_raytracing_shader_table(table_desc);
    REQUIRE(table != nullptr);

    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    auto const input = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const triangles = sg::blas_triangles{.vertices = input.raw(), .vertex_count = 6, .is_opaque = false};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&triangles, 1),
                                                 sg::accel_build_flag::fast_trace, open_t::ray_count);
    auto const offset = table->offset_of(row);
    sg::tlas_instance const instances[] = {
        {.blas = blas, .instance_id = 10, .hit_group_offset = offset},
        {.blas = blas, .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 2}, .instance_id = 20, .hit_group_offset = offset},
    };
    auto const tlas = cmd->raytracing.build_tlas(instances);

    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::traced_open>();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::traced_open{.world = tlas->as_view(),
                                                                                .hits = hits.as_readwrite_buffer(),
                                                                                .ids = ids.as_readwrite_buffer()});
    cmd->raytracing.bind_pipeline(*pipeline);
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), grid, grid);
    auto const ids_back = cmd->download.data_from_buffer(ids);
    ctx->submit_command_list(cc::move(cmd));

    auto const got_ids = co_await ids_back.data();
    REQUIRE(got_ids.size() == grid * grid);
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const want = expected_at(x, y);
            auto const at = y * grid + x;
            auto const where = cc::format("cell ({}, {})", x, y);
            CHECK(got_ids[at][0] == (want.is_hit ? 1 : 0)).context(where);
            if (want.is_hit)
            {
                CHECK(got_ids[at][1] == want.instance_id).context(where);
                CHECK(got_ids[at][3] == (want.is_lit ? 1 : 0)).context(where);
            }
        }
}

// raytracing_callables.sgl's `apply`: thread i calls callable `i % 3` of `ops`, the module's two and then the host's.
// The host's squares, compiled at run time from its own SGL.

namespace
{
constexpr auto k_squared = "require raytracing_pipeline\n"
                           "struct operand:\n    x: float\n"
                           "@callable fun squared(v: mut operand):\n    v.x = v.x * v.x\n";
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL ray-tracing pipeline calls the module's callables and the host's by index",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::raytracing_pipeline))
        SKIP("this device has no ray-tracing pipelines");

    using apply_t = shaders::raytracing_callables_apply_t;
    CHECK(apply_t::first_host_callable == 2);
    auto const squared = co_await slib::compile_callable(ctx.get(), &sg_test::shader_fixtures(), &apply_t::definition(),
                                                         k_squared, "squared", "squared.sgl");
    auto host = slib::raytracing_host_parts();
    host.callables.push_back(squared);
    auto const desc = co_await shaders::raytracing_callables.apply.description(*ctx, host);
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);
    auto const table = ctx->uncached.create_raytracing_shader_table(apply_t::table_description(pipeline, host));
    REQUIRE(table != nullptr);

    constexpr auto count = 9;
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const values = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(count), usage);
    auto cmd = ctx->create_command_list();
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::operands>();
    auto const group
        = ctx->transient.create_binding_group(*cmd, layout, shaders::operands{.values = values.as_readwrite_buffer()});
    cmd->raytracing.bind_pipeline(*pipeline);
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), count);
    auto const back = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == count);
    for (auto i = 0; i < count; ++i)
    {
        auto const x = float(i + 1);
        auto const want = i % 3 == 0 ? 2 * x : i % 3 == 1 ? -x : x * x;
        CHECK(got[i][0] == want).context(cc::format("thread {}", i));
    }
}

// What the host hands over is checked against the declaration before anything compiles, and every refusal is an async
// error of the awaited result, never a throw at the call.

namespace
{
/// The message `node` failed with, or empty where it did not fail.
template <class T>
cc::string error_of(cc::shared_async<T> const& node)
{
    return node->has_error() ? node->try_error()->underlying().to_string() : cc::string();
}

/// `k_runtime_group` with `from` replaced by `to`, once.
cc::string runtime_group_with(cc::string_view from, cc::string_view to)
{
    auto text = cc::string(k_runtime_group);
    auto const at = text.find(from);
    REQUIRE(at >= 0);
    text.replace({.offset = at, .size = from.size()}, to);
    return text;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a host's hit group or callable the declaration cannot take is an async error",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    using open_t = shaders::raytracing_open_open_path_t;
    using apply_t = shaders::raytracing_callables_apply_t;
    auto const* const library = &sg_test::shader_fixtures();
    auto const group = [&](cc::string source, cc::string name)
    { return slib::compile_hit_group(ctx.get(), library, &open_t::definition(), cc::move(source), cc::move(name)); };

    // each is created without a throw, and settles to the error
    auto const missing = group(k_runtime_group, "velvet");
    auto const broken = group("hit_group material for path_rays:\n", "material");
    auto other_set = cc::string(k_runtime_group);
    (void)other_set.replace_all("path_rays", "other_rays");
    auto const renamed = group(other_set, "material");
    auto const swapped = group(runtime_group_with("    surface: radiance\n    occlusion: shadow\n",
                                                  "    occlusion: shadow\n    surface: radiance\n"),
                               "material");
    // the same names, and a payload one field wider than the pipeline's
    auto const widened
        = group(runtime_group_with("    is_lit: int\n", "    is_lit: int\n    extra: float\n"), "material");
    auto const not_open = slib::compile_hit_group(
        ctx.get(), library, &shaders::raytracing_pipeline_path_t::definition(), k_runtime_group, "material");
    auto const other_parameter
        = slib::compile_callable(ctx.get(), library, &apply_t::definition(),
                                 "require raytracing_pipeline\nstruct operand:\n    x: float\n    y: float\n"
                                 "@callable fun squared(v: mut operand):\n    v.x = v.x * v.x\n",
                                 "squared");
    auto const no_callable = slib::compile_callable(ctx.get(), library, &apply_t::definition(), k_squared, "cubed");
    auto const closed = slib::compile_callable(ctx.get(), library, &open_t::definition(), k_squared, "squared");

    for (auto const* node : {&missing, &broken, &renamed, &swapped, &widened, &not_open})
        co_await cc::async_settled(*node);
    for (auto const* node : {&other_parameter, &no_callable, &closed})
        co_await cc::async_settled(*node);

    CHECK(error_of(missing).contains("<generated> declares no hit_group velvet"));
    CHECK(error_of(broken).contains("<generated> does not compile"));
    CHECK(error_of(renamed).contains("is for the ray set other_rays"));
    CHECK(error_of(swapped).contains("whose ray types, payloads and payload layouts it must state alike"));
    CHECK(error_of(widened).contains("whose ray types, payloads and payload layouts it must state alike"));
    CHECK(error_of(not_open).contains("takes no hit group of the host's"));
    CHECK(error_of(other_parameter).contains("@callable squared of <generated> takes operand@"));
    CHECK(error_of(no_callable).contains("<generated> declares no @callable cubed"));
    CHECK(error_of(closed).contains("takes no callable of the host's"));
}

TEST("sg - a table asks for as many callables of the host's as the declaration takes")
{
    using apply_t = shaders::raytracing_callables_apply_t;
    using path_t = shaders::raytracing_pipeline_path_t;
    auto host = slib::raytracing_host_parts();
    host.callables.push_back({});
    // the host's callable takes a record after the module's two
    CHECK(apply_t::table_description({}, host).callable.size() == 3);
    CHECK(apply_t::table_description({}).callable.size() == 2);
    // `path` lists every callable, so a table with one of the host's is a caller's mistake
    CHECK_ASSERTS((void)path_t::table_description({}, host));
}

namespace
{
/// Whether `Pipeline::add_row` takes a `Group` as its hit group.
template <class Pipeline, class Group>
concept takes_row = requires(sg::raytracing_shader_table_description& t, Group g) { Pipeline::add_row(t, g); };
} // namespace

TEST("sg - a generated pipeline's hit group index is its own type")
{
    using path_t = shaders::raytracing_pipeline_path_t;
    using open_t = shaders::raytracing_open_open_path_t;
    static_assert(takes_row<path_t, path_t::hit_group>);
    // another pipeline's group, and a bare position, are no hit group of `path`
    static_assert(!takes_row<path_t, open_t::hit_group>);
    static_assert(!takes_row<path_t, int>);
    CHECK(path_t::hit_groups_t::spheres.index == 1);
    CHECK(open_t::first_host_hit_group.index == 0);
}
