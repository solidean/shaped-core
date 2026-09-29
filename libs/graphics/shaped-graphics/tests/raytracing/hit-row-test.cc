#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/raytracing/impl/hit_record_check.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>


// Hit rows and the hit-record check dispatch_rays runs under context::portability_checks, on CPU stand-ins.
// Neither needs a device: a row is description arithmetic, and the check reads only what sg recorded beside the native objects.
// raytracing-test.cc builds the real structures on every backend and checks that the recording happens.

using namespace cc::primitive_defines;

namespace
{
/// A pipeline that is only its hit groups, which is all the check asks of one.
struct cpu_pipeline final : sg::raytracing_pipeline
{
    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override { return {}; }
};

struct cpu_table final : sg::raytracing_shader_table
{
    explicit cpu_table(sg::raytracing_shader_table_description const& desc) : sg::raytracing_shader_table(desc) {}
};

struct cpu_blas final : sg::blas
{
    explicit cpu_blas(int geometry_count) : sg::blas(0, 0, 0, {}, geometry_count) {}
};

struct cpu_tlas final : sg::tlas
{
    explicit cpu_tlas(int instance_count) : sg::tlas(0, 0, 0, {}, instance_count, {}) {}
};

/// A pipeline with two triangle groups and one procedural group, as handles 0, 1 and 2.
[[nodiscard]] sg::raytracing_pipeline_handle make_pipeline()
{
    auto const pipeline = std::make_shared<cpu_pipeline>();
    sg::hit_shader const groups[3] = {
        {},
        {},
        {.intersection = sg::compiled_shader{.stage = sg::shader_stage::intersection}},
    };
    sg::impl::set_hit_groups(*pipeline, groups);
    return pipeline;
}

constexpr auto k_triangle = sg::hit_shader_handle(0);
constexpr auto k_other_triangle = sg::hit_shader_handle(1);
constexpr auto k_procedural = sg::hit_shader_handle(2);

[[nodiscard]] sg::blas_handle make_blas(sg::blas_geometry geometry, int geometry_count, int stride)
{
    auto const blas = std::make_shared<cpu_blas>(geometry_count);
    sg::impl::set_build_record(*blas, geometry, stride);
    return blas;
}

[[nodiscard]] sg::tlas_handle make_tlas(cc::span<sg::impl::tlas_instance_record const> instances)
{
    auto const tlas = std::make_shared<cpu_tlas>(int(instances.size()));
    sg::impl::set_instance_records(*tlas, cc::vector<sg::impl::tlas_instance_record>::create_copy_of(instances));
    return tlas;
}

[[nodiscard]] sg::tlas_handle make_tlas(sg::impl::tlas_instance_record const& instance)
{
    return make_tlas(cc::span<sg::impl::tlas_instance_record const>(&instance, 1));
}
} // namespace

TEST("sg - a hit row appends one record per ray type, beside single records")
{
    auto desc = sg::raytracing_shader_table_description{.ray_count = 2};

    auto const single = desc.add_hit_shader(k_procedural);
    sg::hit_shader_handle const first[2] = {k_triangle, k_other_triangle};
    auto const row0 = desc.add_hit_row(first);

    // A row may name one group twice, and one group may appear in several rows.
    sg::hit_shader_handle const second[2] = {k_triangle, k_triangle};
    auto const row1 = desc.add_hit_row(second);

    CHECK(u32(single) == 0u);
    CHECK(u32(row0) == 1u);
    CHECK(u32(row1) == 3u);

    REQUIRE(desc.hit.size() == 5);
    CHECK(desc.hit[0] == k_procedural);
    CHECK(desc.hit[1] == k_triangle);
    CHECK(desc.hit[2] == k_other_triangle);
    CHECK(desc.hit[3] == k_triangle);
    CHECK(desc.hit[4] == k_triangle);

    // A row shorter or longer than ray_count is not a row of this table.
    sg::hit_shader_handle const short_row[1] = {k_triangle};
    CHECK_ASSERTS(desc.add_hit_row(short_row));
}

TEST("sg - offset_of is the value an instance takes as its hit_group_offset")
{
    auto desc = sg::raytracing_shader_table_description{.pipeline = make_pipeline(), .ray_count = 3};
    (void)desc.add_hit_shader(k_triangle);
    sg::hit_shader_handle const per_ray_type[3] = {k_triangle, k_other_triangle, k_triangle};
    auto const row = desc.add_hit_row(per_ray_type);

    auto const table = cpu_table(desc);
    CHECK(table.ray_count() == 3);
    CHECK(table.hit_records().size() == 4);
    CHECK(table.offset_of(row) == 1u);

    // A row whose records would run past the table is not one of its rows.
    CHECK_ASSERTS(table.offset_of(sg::hit_row(2)));
}

TEST("sg - the hit-record check is silent when every reached record fits its BLAS")
{
    auto desc = sg::raytracing_shader_table_description{.pipeline = make_pipeline(), .ray_count = 2};
    sg::hit_shader_handle const triangles[2] = {k_triangle, k_other_triangle};
    sg::hit_shader_handle const procedural[2] = {k_procedural, k_procedural};
    auto const triangle_row = desc.add_hit_row(triangles);
    (void)desc.add_hit_row(triangles);
    auto const procedural_row = desc.add_hit_row(procedural);
    auto const table = cpu_table(desc);

    // A two-geometry triangle BLAS reads rows 0 and 1; a one-geometry AABB BLAS reads row 2.
    sg::impl::tlas_instance_record const instances[2] = {
        {.blas = make_blas(sg::blas_geometry::triangles, 2, 2), .hit_group_offset = table.offset_of(triangle_row)},
        {.blas = make_blas(sg::blas_geometry::aabbs, 1, 2), .hit_group_offset = table.offset_of(procedural_row)},
    };
    auto const tlas = make_tlas(instances);

    auto const mismatches = sg::impl::find_hit_record_mismatches(table, *tlas);
    CHECK(mismatches.empty()).context(mismatches.empty() ? cc::string() : mismatches[0]);
}

TEST("sg - the hit-record check names a record of the wrong kind")
{
    auto desc = sg::raytracing_shader_table_description{.pipeline = make_pipeline(), .ray_count = 2};
    sg::hit_shader_handle const row[2] = {k_triangle, k_procedural};
    (void)desc.add_hit_row(row);
    auto const table = cpu_table(desc);

    SECTION("a triangle BLAS reaching a procedural group")
    {
        sg::impl::tlas_instance_record const instance = {.blas = make_blas(sg::blas_geometry::triangles, 1, 2)};
        auto const mismatches = sg::impl::find_hit_record_mismatches(table, *make_tlas(instance));
        REQUIRE(mismatches.size() == 1);
        CHECK(mismatches[0].contains("hit record 1")).context(mismatches[0]);
    }

    SECTION("an AABB BLAS reaching a triangle group")
    {
        sg::impl::tlas_instance_record const instance = {.blas = make_blas(sg::blas_geometry::aabbs, 1, 2)};
        auto const mismatches = sg::impl::find_hit_record_mismatches(table, *make_tlas(instance));
        REQUIRE(mismatches.size() == 1);
        CHECK(mismatches[0].contains("hit record 0")).context(mismatches[0]);
    }

    SECTION("an instance whose mask is 0 is never hit, so it is not checked")
    {
        sg::impl::tlas_instance_record const instance = {.blas = make_blas(sg::blas_geometry::aabbs, 1, 2), .mask = 0};
        CHECK(sg::impl::find_hit_record_mismatches(table, *make_tlas(instance)).empty());
    }
}

TEST("sg - the hit-record check names a record past the table")
{
    auto desc = sg::raytracing_shader_table_description{.pipeline = make_pipeline(), .ray_count = 2};
    sg::hit_shader_handle const row[2] = {k_triangle, k_triangle};
    (void)desc.add_hit_row(row);
    auto const table = cpu_table(desc);

    // The second geometry reads records 2 and 3, and the table holds 2.
    sg::impl::tlas_instance_record const instance = {.blas = make_blas(sg::blas_geometry::triangles, 2, 2)};
    auto const mismatches = sg::impl::find_hit_record_mismatches(table, *make_tlas(instance));
    REQUIRE(mismatches.size() == 2);
    CHECK(mismatches[0].contains("hit record 2")).context(mismatches[0]);
    CHECK(mismatches[1].contains("hit record 3")).context(mismatches[1]);
}

TEST("sg - the hit-record check names a BLAS built for another ray count")
{
    auto desc = sg::raytracing_shader_table_description{.pipeline = make_pipeline(), .ray_count = 2};
    sg::hit_shader_handle const row[2] = {k_triangle, k_triangle};
    (void)desc.add_hit_row(row);
    (void)desc.add_hit_row(row);
    auto const table = cpu_table(desc);

    // Stride 1 fits every record here on dx12, and on metal geometry 1 would read row 0's second record.
    sg::impl::tlas_instance_record const instance = {.blas = make_blas(sg::blas_geometry::triangles, 2, 1)};
    auto const mismatches = sg::impl::find_hit_record_mismatches(table, *make_tlas(instance));
    REQUIRE(mismatches.size() == 1);
    CHECK(mismatches[0].contains("hit_record_stride 1")).context(mismatches[0]);
}

TEST("sg - the hit-record check skips what sg never recorded")
{
    // A pipeline a backend built outside the context says nothing about its groups, so only the range is checked.
    auto desc = sg::raytracing_shader_table_description{.pipeline = std::make_shared<cpu_pipeline>()};
    (void)desc.add_hit_shader(k_procedural);
    auto const table = cpu_table(desc);

    sg::impl::tlas_instance_record const fits = {.blas = make_blas(sg::blas_geometry::triangles, 1, 1)};
    CHECK(sg::impl::find_hit_record_mismatches(table, *make_tlas(fits)).empty());

    sg::impl::tlas_instance_record const overruns = {.blas = make_blas(sg::blas_geometry::triangles, 2, 1)};
    CHECK(sg::impl::find_hit_record_mismatches(table, *make_tlas(overruns)).size() == 1);

    // A tlas built while the checks were off recorded no instances at all.
    auto const unrecorded = std::make_shared<cpu_tlas>(1);
    CHECK(sg::impl::find_hit_record_mismatches(table, *unrecorded).empty());
}
