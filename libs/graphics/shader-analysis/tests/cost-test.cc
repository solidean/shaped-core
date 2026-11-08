#include "spirv-builder.hh"

#include <nexus/test.hh>
#include <shader-analysis/cost.hh>
#include <shader-analysis/metrics.hh>

using namespace cc::primitive_defines;
using namespace shan_test::impl;

namespace
{
constexpr u32 t_int = 40;
constexpr u32 c_zero = 41;
constexpr u32 c_step = 42;
constexpr u32 c_bound = 43;
constexpr u32 t_uint = 44;
constexpr u32 v_id = 45;
constexpr u32 t_input = 46;

[[nodiscard]] shan::function_metrics only_function(spirv_builder const& b)
{
    auto const m = shan::analyze(b.read());
    REQUIRE(m.has_value());
    REQUIRE(m.value().functions.size() == 1);
    return m.value().functions[0];
}

[[nodiscard]] f64 executed(shan::function_metrics const& f, shan::cost_class c)
{
    return f.executed_class_lanes[isize(c)];
}
} // namespace

TEST("shan cost - a loop whose counter states its trips runs its body that often")
{
    // for i in 0 ..< 5: x = x * x
    auto b = spirv_builder();
    declare_basics(b);
    b.op(21, {t_int, 32, 1});
    b.op(43, {t_int, c_zero, 0});
    b.op(43, {t_int, c_step, 1});
    b.op(43, {t_int, c_bound, 5});
    b.op(54, {t_void, 20, 0, t_fn});
    b.op(248, {21});
    b.op(249, {22});
    b.op(248, {22});
    b.op(245, {t_int, 30, c_zero, 21, 31, 24});  // i
    b.op(245, {t_float, 32, c_one, 21, 33, 24}); // x
    b.op(246, {25, 24, 0});                      // LoopMerge
    b.op(177, {t_bool, 34, 30, c_bound});        // i < 5
    b.op(250, {34, 23, 25});
    b.op(248, {23});
    b.op(133, {t_float, 33, 32, 32}); // x * x
    b.op(249, {24});
    b.op(248, {24});
    b.op(128, {t_int, 31, 30, c_step}); // i + 1
    b.op(249, {22});
    b.op(248, {25});
    b.op(62, {g_buffer, 32});
    b.op(253, {});
    b.op(56, {});
    auto const f = only_function(b);
    CHECK(f.loops == 1);
    CHECK(f.counted_loops == 1);
    CHECK(f.executed_lanes[isize(shan::op_kind::float_arithmetic)] == 5.0);
}

TEST("shan cost - a branch on a thread's own value runs both arms, one the wave shares runs each at its share")
{
    for (auto const on_thread : {true, false})
    {
        auto b = spirv_builder();
        b.op(71, {v_id, 11, 28}); // GlobalInvocationId
        declare_basics(b);
        b.op(21, {t_uint, 32, 0});
        b.op(32, {t_input, 1, t_uint});
        b.op(43, {t_uint, c_zero, 0});
        b.op(59, {t_input, v_id, 1});
        b.op(54, {t_void, 20, 0, t_fn});
        b.op(248, {21});
        b.op(61, {t_uint, 22, v_id});
        b.op(61, {t_float, 23, g_buffer});
        if (on_thread)
            b.op(176, {t_bool, 24, 22, c_zero}); // ULessThan on the id
        else
            b.op(184, {t_bool, 24, 23, c_one}); // FOrdLessThan on the buffer
        b.op(247, {53, 0});
        b.op(250, {24, 51, 52});
        b.op(248, {51});
        b.op(133, {t_float, 25, 23, 23});
        b.op(249, {53});
        b.op(248, {52});
        b.op(129, {t_float, 26, 23, c_one});
        b.op(249, {53});
        b.op(248, {53});
        b.op(245, {t_float, 27, 25, 51, 26, 52});
        b.op(62, {g_buffer, 27});
        b.op(253, {});
        b.op(56, {});
        auto const f = only_function(b);
        // a multiply in one arm and an add in the other
        CHECK(f.executed_lanes[isize(shan::op_kind::float_arithmetic)] == (on_thread ? 2.0 : 1.0));
    }
}

TEST("shan cost - a division is one transcendental a lane, and the model weighs each class at its rate")
{
    auto b = spirv_builder();
    declare_basics(b);
    b.op(54, {t_void, 20, 0, t_fn});
    b.op(248, {21});
    b.op(61, {t_float4, 22, g_buffer});
    b.op(136, {t_float4, 23, 22, 22}); // FDiv of four lanes
    b.op(81, {t_float, 24, 23, 0});
    b.op(62, {g_buffer, 24});
    b.op(253, {});
    b.op(56, {});
    auto const f = only_function(b);
    // as both GPUs measured it: a division costs what a square root does, and nothing beside
    CHECK(executed(f, shan::cost_class::alu) == 0.0);
    CHECK(executed(f, shan::cost_class::transcendental) == 4.0);
    auto model = shan::cost_model{.name = "test"};
    model.per_lane[isize(shan::cost_class::alu)] = 1.0;
    model.per_lane[isize(shan::cost_class::transcendental)] = 4.0;
    CHECK(shan::compute_cost(f, model) == 16.0);
}
