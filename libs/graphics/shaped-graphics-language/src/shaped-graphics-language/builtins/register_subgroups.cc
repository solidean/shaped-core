#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>

using namespace sgl;
using namespace sgl::builtins;

// The subgroup operations (the spec's checking file, "Subgroups"), each the target's own by EMIT-146's table.

namespace
{
/// Gives nothing, which the interpreter takes for a wrong result: a test runs one invocation, which has no subgroup,
/// and one that reaches a subgroup operation is refused before it runs (CHK-379).
void no_subgroup(cc::span<check::scalar const>, cc::vector<check::scalar>&)
{
}

enum class subgroup_op : u8
{
    all,
    any,
    ballot,
    add,
    mul,
    min,
    max,
    bit_and,
    bit_or,
    bit_xor,
    exclusive_add,
    exclusive_mul,
    inclusive_add,
    inclusive_mul,
    broadcast,
    broadcast_first,
    shuffle,
    shuffle_xor,
    shuffle_up,
    shuffle_down,
    quad_swap_x,
    quad_swap_y,
    quad_swap_diagonal,
    quad_broadcast,
};

/// What an operation takes besides the value it exchanges.
enum class operand : u8
{
    /// One `bool`.
    vote,
    /// A number, scalar or vector.
    number,
    /// An integer, scalar or vector.
    integer,
    /// A number and an `int` naming an invocation or an offset.
    number_and_int,
};

struct subgroup_info
{
    subgroup_op op;
    cc::string_view name;
    operand takes;
    /// The second parameter's name, where it takes one.
    cc::string_view second;
    cc::string_view hlsl;
    cc::string_view wgsl;
    cc::string_view msl;
    cc::string_view doc;
};

constexpr subgroup_info k_operations[] = {
    {subgroup_op::all, "subgroup_all", operand::vote, "", "WaveActiveAllTrue", "subgroupAll", "simd_all",
     "/// Whether `b` holds in every invocation of the subgroup."},
    {subgroup_op::any, "subgroup_any", operand::vote, "", "WaveActiveAnyTrue", "subgroupAny", "simd_any",
     "/// Whether `b` holds in any invocation of the subgroup."},
    {subgroup_op::ballot, "subgroup_ballot", operand::vote, "", "WaveActiveBallot", "subgroupBallot",
     "sgl_subgroup_ballot", "/// Bit `i` set where invocation `i`'s `b` holds."},
    {subgroup_op::add, "subgroup_add", operand::number, "", "WaveActiveSum", "subgroupAdd", "simd_sum",
     "/// The sum of `x` over the subgroup."},
    {subgroup_op::mul, "subgroup_mul", operand::number, "", "WaveActiveProduct", "subgroupMul", "simd_product",
     "/// The product of `x` over the subgroup."},
    {subgroup_op::min, "subgroup_min", operand::number, "", "WaveActiveMin", "subgroupMin", "simd_min",
     "/// The minimum of `x` over the subgroup."},
    {subgroup_op::max, "subgroup_max", operand::number, "", "WaveActiveMax", "subgroupMax", "simd_max",
     "/// The maximum of `x` over the subgroup."},
    {subgroup_op::bit_and, "subgroup_bit_and", operand::integer, "", "WaveActiveBitAnd", "subgroupAnd", "simd_and",
     "/// The bits `x` has in every invocation of the subgroup."},
    {subgroup_op::bit_or, "subgroup_bit_or", operand::integer, "", "WaveActiveBitOr", "subgroupOr", "simd_or",
     "/// The bits `x` has in any invocation of the subgroup."},
    {subgroup_op::bit_xor, "subgroup_bit_xor", operand::integer, "", "WaveActiveBitXor", "subgroupXor", "simd_xor",
     "/// The bits `x` has in an odd number of invocations of the subgroup."},
    {subgroup_op::exclusive_add, "subgroup_exclusive_add", operand::number, "", "WavePrefixSum", "subgroupExclusiveAdd",
     "simd_prefix_exclusive_sum", "/// The sum of `x` over the invocations below this one."},
    {subgroup_op::exclusive_mul, "subgroup_exclusive_mul", operand::number, "", "WavePrefixProduct",
     "subgroupExclusiveMul", "simd_prefix_exclusive_product",
     "/// The product of `x` over the invocations below this one."},
    {subgroup_op::inclusive_add, "subgroup_inclusive_add", operand::number, "", "sgl_subgroup_inclusive_add",
     "subgroupInclusiveAdd", "simd_prefix_inclusive_sum",
     "/// The sum of `x` over the invocations below this one and this one."},
    {subgroup_op::inclusive_mul, "subgroup_inclusive_mul", operand::number, "", "sgl_subgroup_inclusive_mul",
     "subgroupInclusiveMul", "simd_prefix_inclusive_product",
     "/// The product of `x` over the invocations below this one and this one."},
    {subgroup_op::broadcast, "subgroup_broadcast", operand::number_and_int, "lane", "WaveReadLaneAt",
     "subgroupBroadcast", "simd_broadcast", "/// `x` of the invocation `lane`, a constant from 0 to 127 (CHK-378)."},
    {subgroup_op::broadcast_first, "subgroup_broadcast_first", operand::number, "", "WaveReadLaneFirst",
     "subgroupBroadcastFirst", "simd_broadcast_first", "/// `x` of the first active invocation of the subgroup."},
    {subgroup_op::shuffle, "subgroup_shuffle", operand::number_and_int, "lane", "WaveReadLaneAt", "subgroupShuffle",
     "simd_shuffle", "/// `x` of the invocation `lane`, computed at run time."},
    {subgroup_op::shuffle_xor, "subgroup_shuffle_xor", operand::number_and_int, "mask", "WaveReadLaneAt",
     "subgroupShuffleXor", "simd_shuffle_xor", "/// `x` of the invocation whose index is this one's `^ mask`."},
    {subgroup_op::shuffle_up, "subgroup_shuffle_up", operand::number_and_int, "delta", "WaveReadLaneAt",
     "subgroupShuffleUp", "simd_shuffle_up", "/// `x` of the invocation `delta` below this one."},
    {subgroup_op::shuffle_down, "subgroup_shuffle_down", operand::number_and_int, "delta", "WaveReadLaneAt",
     "subgroupShuffleDown", "simd_shuffle_down", "/// `x` of the invocation `delta` above this one."},
    {subgroup_op::quad_swap_x, "quad_swap_x", operand::number, "", "QuadReadAcrossX", "quadSwapX", "quad_shuffle_xor",
     "/// `x` of the quad's horizontal neighbour."},
    {subgroup_op::quad_swap_y, "quad_swap_y", operand::number, "", "QuadReadAcrossY", "quadSwapY", "quad_shuffle_xor",
     "/// `x` of the quad's vertical neighbour."},
    {subgroup_op::quad_swap_diagonal, "quad_swap_diagonal", operand::number, "", "QuadReadAcrossDiagonal",
     "quadSwapDiagonal", "quad_shuffle_xor", "/// `x` of the quad's diagonal neighbour."},
    {subgroup_op::quad_broadcast, "quad_broadcast", operand::number_and_int, "lane", "QuadReadLaneAt", "quadBroadcast",
     "quad_broadcast", "/// `x` of the quad's invocation `lane`, a constant from 0 to 3 (CHK-378)."},
};

// Every name the writers below spell, by target: a name of the program spelled alike would hide it (EMIT-15).
constexpr cc::string_view k_hlsl_names[] = {
    "WaveActiveAllTrue",
    "WaveActiveAnyTrue",
    "WaveActiveBallot",
    "WaveActiveSum",
    "WaveActiveProduct",
    "WaveActiveMin",
    "WaveActiveMax",
    "WaveActiveBitAnd",
    "WaveActiveBitOr",
    "WaveActiveBitXor",
    "WavePrefixSum",
    "WavePrefixProduct",
    "WaveReadLaneAt",
    "WaveReadLaneFirst",
    "WaveGetLaneIndex",
    "WaveGetLaneCount",
    "QuadReadAcrossX",
    "QuadReadAcrossY",
    "QuadReadAcrossDiagonal",
    "QuadReadLaneAt",
    "sgl_subgroup_inclusive_add",
    "sgl_subgroup_inclusive_mul",
};
constexpr cc::string_view k_wgsl_names[] = {
    "subgroupAll",
    "subgroupAny",
    "subgroupBallot",
    "subgroupAdd",
    "subgroupMul",
    "subgroupMin",
    "subgroupMax",
    "subgroupAnd",
    "subgroupOr",
    "subgroupXor",
    "subgroupExclusiveAdd",
    "subgroupExclusiveMul",
    "subgroupInclusiveAdd",
    "subgroupInclusiveMul",
    "subgroupBroadcast",
    "subgroupBroadcastFirst",
    "subgroupShuffle",
    "subgroupShuffleXor",
    "subgroupShuffleUp",
    "subgroupShuffleDown",
    "quadSwapX",
    "quadSwapY",
    "quadSwapDiagonal",
    "quadBroadcast",
};
constexpr cc::string_view k_msl_names[] = {
    "simd_all",
    "simd_any",
    "simd_ballot",
    "sgl_subgroup_ballot",
    "simd_sum",
    "simd_product",
    "simd_min",
    "simd_max",
    "simd_and",
    "simd_or",
    "simd_xor",
    "simd_prefix_exclusive_sum",
    "simd_prefix_exclusive_product",
    "simd_prefix_inclusive_sum",
    "simd_prefix_inclusive_product",
    "simd_broadcast",
    "simd_broadcast_first",
    "simd_shuffle",
    "simd_shuffle_xor",
    "simd_shuffle_up",
    "simd_shuffle_down",
    "quad_shuffle_xor",
    "quad_broadcast",
};

/// The unsigned integer type of the width and component count of the signed one `language` spells `type`; empty where
/// `type` is no signed integer type.
cc::string_view unsigned_twin(registry const& r, language l, cc::string_view type)
{
    for (auto const& t : r.types)
    {
        if (t.spelled_in(l) != type)
            continue;
        if (t.leaf_kind != check::value_kind::scalar_int && t.leaf_kind != check::value_kind::scalar_short)
            return {};
        auto const kind = t.leaf_kind == check::value_kind::scalar_int ? check::value_kind::scalar_uint
                                                                       : check::value_kind::scalar_ushort;
        for (auto const& u : r.types)
            if (u.leaf_kind == kind && u.leaf_count == t.leaf_count)
                return u.spelled_in(l);
        return {};
    }
    return {};
}

/// EMIT-146: the target's own operation, a lane converted to the target's lane type.
written write_subgroup(call_context const& ctx)
{
    auto const& info = k_operations[ctx.data];
    auto const& x = ctx.arguments[0].text;
    auto const name = ctx.target == language::hlsl ? info.hlsl : ctx.target == language::wgsl ? info.wgsl : info.msl;
    if (info.takes != operand::number_and_int)
    {
        // MSL has one quad shuffle, by the lane's bits that differ
        if (ctx.target == language::msl
            && (info.op == subgroup_op::quad_swap_x || info.op == subgroup_op::quad_swap_y
                || info.op == subgroup_op::quad_swap_diagonal))
            return {.text = cc::format("quad_shuffle_xor({}, {})", x,
                                       info.op == subgroup_op::quad_swap_x   ? 1
                                       : info.op == subgroup_op::quad_swap_y ? 2
                                                                             : 3)};
        // HLSL's bitwise reductions take unsigned integers alone
        if (ctx.target == language::hlsl
            && (info.op == subgroup_op::bit_and || info.op == subgroup_op::bit_or || info.op == subgroup_op::bit_xor))
            if (auto const as_unsigned = unsigned_twin(ctx.builtins, ctx.target, ctx.result_type); !as_unsigned.empty())
                return {.text = cc::format("{}({}({}({})))", ctx.result_type, name, as_unsigned, x)};
        return {.text = cc::format("{}({})", name, x)};
    }
    auto const lane_type = ctx.target == language::hlsl ? "uint" : ctx.target == language::wgsl ? "u32" : "ushort";
    auto const lane = cc::format("{}({})", lane_type, ctx.arguments[1].text);
    // HLSL reads a shuffle by an offset at the lane it computes from this invocation's own
    if (ctx.target == language::hlsl && info.op != subgroup_op::broadcast && info.op != subgroup_op::shuffle
        && info.op != subgroup_op::quad_broadcast)
        return {.text = cc::format("WaveReadLaneAt({}, WaveGetLaneIndex() {} {})", x,
                                   info.op == subgroup_op::shuffle_xor  ? "^"
                                   : info.op == subgroup_op::shuffle_up ? "-"
                                                                        : "+",
                                   lane)};
    return {.text = cc::format("{}({}, {})", name, x, lane)};
}

/// HLSL has no inclusive prefix, which is the exclusive one combined with the invocation's own value.
/// MSL's ballot is 64 bits, which fill the low two components of the `uint4`.
cc::string subgroup_helper(helper_context const& c)
{
    auto const& info = k_operations[c.data];
    if (c.target == language::hlsl && (info.op == subgroup_op::inclusive_add || info.op == subgroup_op::inclusive_mul))
        return cc::format("{0} {1}({0} x)\n"
                          "{{\n"
                          "    return {2}(x) {3} x;\n"
                          "}}\n",
                          c.argument_types[0], info.hlsl,
                          info.op == subgroup_op::inclusive_add ? "WavePrefixSum" : "WavePrefixProduct",
                          info.op == subgroup_op::inclusive_add ? "+" : "*");
    if (c.target == language::msl && info.op == subgroup_op::ballot)
        return "uint4 sgl_subgroup_ballot(bool b)\n"
               "{\n"
               "    ulong v = ulong(simd_ballot(b));\n"
               "    return uint4(uint(v), uint(v >> 32), 0u, 0u);\n"
               "}\n";
    return {};
}
} // namespace

void sgl::builtins::register_subgroups(registry& r)
{
    r.add_comment("// Subgroup operations, which exchange values between the invocations the hardware runs together.\n"
                  "// Each needs `subgroups`, stands only in uniform control flow, and gives a value that differs\n"
                  "// within a workgroup (CHK-376, CHK-377).");
    // a number is any numeric scalar of the prelude or a vector of them, the 16-bit ones too
    cc::string_view const numbers[]
        = {"float", "float2", "float3", "float4", "int",    "int2",    "int3",    "int4",
           "uint",  "uint2",  "uint3",  "uint4",  "half",   "half2",   "half3",   "half4",
           "short", "short2", "short3", "short4", "ushort", "ushort2", "ushort3", "ushort4"};
    cc::string_view const integers[]
        = {"int",   "int2",   "int3",   "int4",   "uint",   "uint2",   "uint3",   "uint4",
           "short", "short2", "short3", "short4", "ushort", "ushort2", "ushort3", "ushort4"};
    cc::string_view const votes[] = {"bool"};
    for (auto i = u32(0); i < u32(sizeof(k_operations) / sizeof(k_operations[0])); ++i)
    {
        auto const& info = k_operations[i];
        auto const types = info.takes == operand::vote    ? cc::span<cc::string_view const>(votes)
                         : info.takes == operand::integer ? cc::span<cc::string_view const>(integers)
                                                          : cc::span<cc::string_view const>(numbers);
        for (auto const type : types)
        {
            auto const result = info.op == subgroup_op::ballot                             ? cc::string_view("uint4")
                              : info.op == subgroup_op::all || info.op == subgroup_op::any ? cc::string_view("bool")
                                                                                           : type;
            auto const parameters = info.takes == operand::vote ? cc::format("b: {}", type)
                                  : info.takes == operand::number_and_int
                                      ? cc::format("x: {}, {}: int", type, info.second)
                                      : cc::format("x: {}", type);
            auto const has_helper = info.op == subgroup_op::inclusive_add || info.op == subgroup_op::inclusive_mul
                                 || info.op == subgroup_op::ballot;
            r.add(function_record{
                .signature = cc::format("@stages(.pixel, .compute) fun {}({}) -> {}", info.name, parameters, result),
                .doc = info.doc,
                .evaluate = no_subgroup,
                .write = {.kind = spelling_kind::custom,
                          .custom = write_subgroup,
                          .helper = has_helper ? subgroup_helper : nullptr,
                          .data = i,
                          .hlsl_names = k_hlsl_names,
                          .wgsl_names = k_wgsl_names,
                          .msl_names = k_msl_names},
                .is_subgroup_operation = true,
                .is_quad_operation = info.name.starts_with("quad_"),
                .constant_lane_below = info.op == subgroup_op::broadcast || info.op == subgroup_op::shuffle_xor
                                            || info.op == subgroup_op::shuffle_up || info.op == subgroup_op::shuffle_down
                                         ? 128
                                     : info.op == subgroup_op::quad_broadcast ? 4
                                                                              : 0,
                .features = check::feature_set(check::feature::subgroups),
            });
        }
    }
}
