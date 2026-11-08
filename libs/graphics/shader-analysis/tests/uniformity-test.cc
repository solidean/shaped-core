#include "spirv-builder.hh"

#include <clean-core/streams/file_stream.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shader-analysis/metrics.hh>

using namespace cc::primitive_defines;
using namespace shan_test::impl;

namespace
{
// a uint, a uint3 Input variable of builtin `builtin`, the pointers into it, and a uint 0
constexpr u32 t_uint = 40;
constexpr u32 v_builtin = 41;
constexpr u32 t_uint3 = 42;
constexpr u32 t_input3 = 43;
constexpr u32 t_input = 44;
constexpr u32 c_zero = 45;

void declare_builtin(spirv_builder& b, u32 builtin)
{
    b.op(71, {v_builtin, 11, builtin}); // OpDecorate BuiltIn
    declare_basics(b);
    b.op(21, {t_uint, 32, 0});
    b.op(23, {t_uint3, t_uint, 3});
    b.op(32, {t_input3, 1, t_uint3}); // Input
    b.op(32, {t_input, 1, t_uint});
    b.op(43, {t_uint, c_zero, 0});
    b.op(59, {t_input3, v_builtin, 1});
}

/// `target = float(builtin.x) + buffer`: the builtin's lane and a buffer's float, both live at the add.
[[nodiscard]] shan::function_metrics add_builtin_to_buffer(u32 builtin)
{
    auto b = spirv_builder();
    declare_builtin(b, builtin);
    b.op(54, {t_void, 20, 0, t_fn});
    b.op(248, {21});
    b.op(65, {t_input, 22, v_builtin, c_zero}); // OpAccessChain: .x
    b.op(61, {t_uint, 23, 22});
    b.op(61, {t_float, 24, g_buffer});
    b.op(112, {t_float, 25, 23}); // OpConvertUToF
    b.op(129, {t_float, 26, 25, 24});
    b.op(62, {g_buffer, 26});
    b.op(253, {});
    b.op(56, {});
    auto const m = shan::analyze(b.read());
    REQUIRE(m.has_value());
    return m.value().functions[0];
}

/// A phi of two values the wave shares, after a branch on the builtin's lane where `on_builtin`, on a buffer's float
/// otherwise.
[[nodiscard]] shan::function_metrics join_after_branch(bool on_builtin)
{
    auto b = spirv_builder();
    declare_builtin(b, 28); // GlobalInvocationId
    b.op(54, {t_void, 20, 0, t_fn});
    b.op(248, {21});
    b.op(65, {t_input, 22, v_builtin, c_zero});
    b.op(61, {t_uint, 23, 22});
    b.op(61, {t_float, 24, g_buffer});
    if (on_builtin)
        b.op(176, {t_bool, 30, 23, c_zero}); // OpULessThan
    else
        b.op(184, {t_bool, 30, 24, c_one}); // OpFOrdLessThan
    b.op(247, {53, 0});
    b.op(250, {30, 51, 52});
    b.op(248, {51});
    b.op(249, {53});
    b.op(248, {52});
    b.op(249, {53});
    b.op(248, {53});
    b.op(245, {t_float, 35, c_one, 51, 24, 52});
    b.op(62, {g_buffer, 35});
    b.op(253, {});
    b.op(56, {});
    auto const m = shan::analyze(b.read());
    REQUIRE(m.has_value());
    return m.value().functions[0];
}
} // namespace

TEST("shan analyze - a value derived from a thread's id is a vector word, one a buffer gives the wave a scalar word")
{
    auto const f = add_builtin_to_buffer(28); // GlobalInvocationId
    CHECK(f.max_live_words == 2);
    CHECK(f.max_live_vector_words == 1);
    CHECK(f.max_live_scalar_words == 1);
}

TEST("shan analyze - a workgroup's id is the same across its wave, so what derives from it is scalar")
{
    auto const f = add_builtin_to_buffer(26); // WorkgroupId
    CHECK(f.max_live_words == 2);
    CHECK(f.max_live_vector_words == 0);
    CHECK(f.max_live_scalar_words == 2);
}

TEST("shan analyze - a phi of shared values is a vector word only where a branch on a thread's own value parted the "
     "lanes")
{
    auto const varying = join_after_branch(true);
    auto const uniform = join_after_branch(false);
    // the phi is all that is live at the store
    CHECK(varying.max_live_vector_words == 1);
    CHECK(uniform.max_live_vector_words == 0);
    CHECK(uniform.max_live_scalar_words >= 1);
}

TEST("shan analyze - a loop whose exit a thread's id decides makes its counter a vector word, in DXIL too")
{
    // tiny_cs.dxil's loop runs to `id.y`, so even its counter, which starts at 0 and adds 1, differs across lanes
    auto file = cc::file_read_stream_adapter::open(nx::source_path(SHAN_DXIL_DATA_DIR "/tiny_cs.dxil"));
    REQUIRE(file.has_value());
    auto stream = file.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    auto const m = shan::analyze_binary(bytes.value());
    REQUIRE(m.has_value());
    auto const& f = m.value().functions[0];
    CHECK(f.max_live_vector_words > 0);
    CHECK(f.max_live_vector_words == f.max_live_words);
}
