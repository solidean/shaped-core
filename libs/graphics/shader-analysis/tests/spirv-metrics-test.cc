#include "spirv-builder.hh"

#include <nexus/test.hh>
#include <shader-analysis/metrics.hh>

using namespace cc::primitive_defines;
using namespace shan_test::impl;

TEST("shan analyze - one block counts its operations, its live words and its chain")
{
    auto b = spirv_builder();
    declare_basics(b);
    b.name(20, "main");
    b.op(54, {t_void, 20, 0, t_fn});
    b.op(248, {21});
    b.op(61, {t_float, 22, g_buffer});   // x = load
    b.op(129, {t_float, 23, 22, c_one}); // y = x + 1
    b.op(133, {t_float, 24, 23, 22});    // z = y * x: x and y both live here
    b.op(62, {g_buffer, 24});
    b.op(253, {});
    b.op(56, {});
    auto const m = shan::analyze(b.read());
    REQUIRE(m.has_value());
    REQUIRE(m.value().functions.size() == 1);
    auto const& f = m.value().functions[0];
    CHECK(f.name == "main");
    CHECK(f.blocks == 1);
    CHECK(f.count_of(shan::op_kind::load) == 1);
    CHECK(f.count_of(shan::op_kind::float_arithmetic) == 2);
    CHECK(f.count_of(shan::op_kind::store) == 1);
    CHECK(f.instructions == 5); // load, add, mul, store, return
    CHECK(f.max_live_words == 2);
    CHECK(f.critical_path == shan::latency_of(shan::op_kind::load) + 2);
    CHECK(m.value().total.instructions == 5);
}

TEST("shan analyze - a value read past a branch is live across it, and a phi's operand is live out of the block it "
     "names")
{
    auto b = spirv_builder();
    declare_basics(b);
    b.op(54, {t_void, 20, 0, t_fn});
    b.op(248, {21});
    b.op(61, {t_float4, 30, g_buffer}); // a float4, live into both arms
    b.op(61, {t_float, 31, g_buffer});
    b.op(184, {t_bool, 32, 31, c_one}); // FOrdLessThan
    b.op(247, {25, 0});                 // SelectionMerge
    b.op(250, {32, 22, 23});
    b.op(248, {22});
    b.op(81, {t_float, 33, 30, 0}); // lane 0: the literal 0 is no id
    b.op(249, {25});
    b.op(248, {23});
    b.op(81, {t_float, 34, 30, 1});
    b.op(249, {25});
    b.op(248, {25});
    b.op(245, {t_float, 35, 33, 22, 34, 23});
    b.op(62, {g_buffer, 35});
    b.op(253, {});
    b.op(56, {});
    auto const m = shan::analyze(b.read());
    REQUIRE(m.has_value());
    auto const& f = m.value().functions[0];
    CHECK(f.blocks == 4);
    CHECK(f.selections == 1);
    CHECK(f.count_of(shan::op_kind::phi) == 1);
    // the float4 and the float the compare reads: 5 words where both are live
    CHECK(f.max_live_words == 5);
}

TEST("shan analyze - a function's variables count as words of their own, and debug info counts as no operation")
{
    auto b = spirv_builder();
    declare_basics(b);
    b.op(32, {40, 7, t_float4}); // a Function pointer to a float4
    b.op(54, {t_void, 20, 0, t_fn});
    b.op(248, {21});
    b.op(59, {40, 41, 7});
    b.op(8, {1, 2, 3}); // OpLine
    b.op(253, {});
    b.op(56, {});
    auto const m = shan::analyze(b.read());
    REQUIRE(m.has_value());
    auto const& f = m.value().functions[0];
    CHECK(f.variable_words == 4);
    CHECK(f.instructions == 2); // the variable and the return
}

TEST("shan analyze - a block with no terminator is an error")
{
    auto b = spirv_builder();
    declare_basics(b);
    b.op(54, {t_void, 20, 0, t_fn});
    b.op(248, {21});
    b.op(61, {t_float, 22, g_buffer});
    b.op(56, {});
    CHECK(shan::analyze(b.read()).has_error());
}
