#include <nexus/test.hh>
#include <shaped-graphics/barrier/access_inference.hh>

// Where a declared array element is tracked, pinned without a GPU.
// A readback cannot pin it: a barrier at the wrong stage races, and a race usually still reads back right.

using sg::access_flag;
using sg::pipeline_stage_flag;

namespace
{
constexpr auto draw_stages = pipeline_stage_flag::vertex | pipeline_stage_flag::fragment;

constexpr auto declared_read
    = sg::impl::array_declarations{.named = true, .any_element = true, .access = access_flag::shader_read};

// The log-once key is the pipeline's address, so each test logs against one of its own.
int pipeline_a = 0;
int pipeline_b = 0;
int pipeline_c = 0;
int pipeline_d = 0;
} // namespace

TEST("sg array plan - a declared element is tracked where the code touches its array")
{
    auto const fragment_only
        = sg::impl::slot_use{.access = access_flag::shader_read, .stages = pipeline_stage_flag::fragment};
    auto const plan = sg::impl::plan_array_declarations(&pipeline_a, "Table", fragment_only, sg::view_class::readonly,
                                                        draw_stages, declared_read);
    CHECK(plan.how == sg::impl::array_plan::mode::as_declared);
    CHECK(plan.stages == sg::pipeline_stage_flags(pipeline_stage_flag::fragment));
}

TEST("sg array plan - without a footprint a declared element is tracked at every stage of the op")
{
    auto const plan = sg::impl::plan_array_declarations(&pipeline_b, "Table", {}, sg::view_class::readonly, draw_stages,
                                                        declared_read);
    CHECK(plan.how == sg::impl::array_plan::mode::as_declared);
    CHECK(plan.stages == draw_stages);
}

TEST("sg array plan - a use outside the op's stages falls back to the op's")
{
    auto const compute_only
        = sg::impl::slot_use{.access = access_flag::shader_read, .stages = pipeline_stage_flag::compute};
    auto const plan = sg::impl::plan_array_declarations(&pipeline_c, "Table", compute_only, sg::view_class::readonly,
                                                        draw_stages, declared_read);
    CHECK(plan.stages == draw_stages);
}

TEST("sg array plan - an undeclared array is covered where the code touches it")
{
    auto const fragment_only
        = sg::impl::slot_use{.access = access_flag::shader_read, .stages = pipeline_stage_flag::fragment};
    nx::expect_error("declared no access for a bound array", nx::exactly(1));
    auto const plan = sg::impl::plan_array_declarations(&pipeline_d, "Table", fragment_only, sg::view_class::readonly,
                                                        draw_stages, sg::impl::array_declarations());
    CHECK(plan.how == sg::impl::array_plan::mode::cover_all);
    CHECK(plan.stages == sg::pipeline_stage_flags(pipeline_stage_flag::fragment));
}
