#include "writer.hh"

#include <clean-core/common/assert.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::spirv::impl;

namespace
{
/// GLSL.std.450's number of each `math`, parallel to it.
constexpr u32 k_glsl_numbers[] = {
    2,  3,  4,  5,  6,  7,  8,  9,  10, // round_even to fract
    13, 14, 15, 16, 17, 18, 19, 20, 21, // sin to tanh
    25, 26, 27, 28, 29, 30, 31, 32, 33, // atan2 to determinant
    37, 38, 39, 40, 41, 42, 43, 44, 45, // fmin to sclamp
    46, 48, 49, 50,                     // fmix to fma
    54, 55, 58, 62, 63, 64,             // the packs
    66, 67, 68, 69, 71, 72,             // length to refract
    73, 74, 75,                         // find_ilsb to find_umsb
};
static_assert(sizeof(k_glsl_numbers) / sizeof(k_glsl_numbers[0]) == sizeof(char[isize(math::find_umsb) + 1]));

/// How an operation of the form is written.
enum class rule_kind : u8
{
    /// A terminator, which `terminator` writes.
    none,
    /// Its result and then its operands.
    plain,
    math,
    /// Its result, then values and literals as `operand_role` tells them apart.
    composite,
    store,
    array_length,
    stage_load,
    stage_store,
    atomic,
    atomic_fadd,
    barrier,
    discard,
    derivative,
    sample_lod,
    image_fetch,
    image_gather,
    image_write,
    image_size,
    subgroup,
    phi,
};

struct op_rule
{
    op code = op::unreachable;
    rule_kind kind = rule_kind::none;
    u32 opcode = 0;
    /// A guarantee stricter than `fast` decorates it `NoContraction`.
    bool keeps_contraction = false;
};

constexpr op_rule k_rules[] = {
    {op::fadd, rule_kind::plain, spv::FAdd, true},
    {op::fsub, rule_kind::plain, spv::FSub, true},
    {op::fmul, rule_kind::plain, spv::FMul, true},
    {op::fdiv, rule_kind::plain, spv::FDiv, true},
    {op::frem, rule_kind::plain, spv::FRem, true},
    {op::fneg, rule_kind::plain, spv::FNegate, true},
    {op::iadd, rule_kind::plain, spv::IAdd},
    {op::isub, rule_kind::plain, spv::ISub},
    {op::imul, rule_kind::plain, spv::IMul},
    {op::sdiv, rule_kind::plain, spv::SDiv},
    {op::udiv, rule_kind::plain, spv::UDiv},
    {op::srem, rule_kind::plain, spv::SRem},
    {op::urem, rule_kind::plain, spv::UMod},
    {op::ineg, rule_kind::plain, spv::SNegate},
    {op::bit_and, rule_kind::plain, spv::BitwiseAnd},
    {op::bit_or, rule_kind::plain, spv::BitwiseOr},
    {op::bit_xor, rule_kind::plain, spv::BitwiseXor},
    {op::bit_not, rule_kind::plain, spv::Not},
    {op::bit_reverse, rule_kind::plain, spv::BitReverse},
    {op::bit_count, rule_kind::plain, spv::BitCount},
    {op::shift_left, rule_kind::plain, spv::ShiftLeftLogical},
    {op::shift_right_logical, rule_kind::plain, spv::ShiftRightLogical},
    {op::shift_right_arithmetic, rule_kind::plain, spv::ShiftRightArithmetic},
    {op::feq, rule_kind::plain, spv::FOrdEqual},
    {op::fne, rule_kind::plain, spv::FUnordNotEqual},
    {op::flt, rule_kind::plain, spv::FOrdLessThan},
    {op::fle, rule_kind::plain, spv::FOrdLessThanEqual},
    {op::fgt, rule_kind::plain, spv::FOrdGreaterThan},
    {op::fge, rule_kind::plain, spv::FOrdGreaterThanEqual},
    {op::is_nan, rule_kind::plain, spv::IsNan},
    {op::is_inf, rule_kind::plain, spv::IsInf},
    {op::ieq, rule_kind::plain, spv::IEqual},
    {op::ine, rule_kind::plain, spv::INotEqual},
    {op::slt, rule_kind::plain, spv::SLessThan},
    {op::sle, rule_kind::plain, spv::SLessThanEqual},
    {op::sgt, rule_kind::plain, spv::SGreaterThan},
    {op::sge, rule_kind::plain, spv::SGreaterThanEqual},
    {op::ult, rule_kind::plain, spv::ULessThan},
    {op::ule, rule_kind::plain, spv::ULessThanEqual},
    {op::ugt, rule_kind::plain, spv::UGreaterThan},
    {op::uge, rule_kind::plain, spv::UGreaterThanEqual},
    {op::logical_and, rule_kind::plain, spv::LogicalAnd},
    {op::logical_or, rule_kind::plain, spv::LogicalOr},
    {op::logical_not, rule_kind::plain, spv::LogicalNot},
    {op::logical_eq, rule_kind::plain, spv::LogicalEqual},
    {op::logical_ne, rule_kind::plain, spv::LogicalNotEqual},
    {op::any, rule_kind::plain, spv::Any},
    {op::all, rule_kind::plain, spv::All},
    {op::select, rule_kind::plain, spv::Select},
    {op::f_to_s, rule_kind::plain, spv::ConvertFToS},
    {op::f_to_u, rule_kind::plain, spv::ConvertFToU},
    {op::s_to_f, rule_kind::plain, spv::ConvertSToF},
    {op::u_to_f, rule_kind::plain, spv::ConvertUToF},
    {op::f_resize, rule_kind::plain, spv::FConvert},
    {op::s_resize, rule_kind::plain, spv::SConvert},
    {op::u_resize, rule_kind::plain, spv::UConvert},
    {op::bitcast, rule_kind::plain, spv::Bitcast},
    {op::copy_logical, rule_kind::plain, spv::CopyLogical},
    {op::dot, rule_kind::plain, spv::Dot, true},
    {op::vector_times_scalar, rule_kind::plain, spv::VectorTimesScalar, true},
    {op::matrix_times_scalar, rule_kind::plain, spv::MatrixTimesScalar, true},
    {op::matrix_times_vector, rule_kind::plain, spv::MatrixTimesVector, true},
    {op::matrix_times_matrix, rule_kind::plain, spv::MatrixTimesMatrix, true},
    {op::transpose, rule_kind::plain, spv::Transpose},
    {op::construct, rule_kind::plain, spv::CompositeConstruct},
    {op::load, rule_kind::plain, spv::Load},
    {op::access_chain, rule_kind::plain, spv::AccessChain},
    {op::combine_sampler, rule_kind::plain, spv::SampledImage},
    {op::image_read, rule_kind::plain, spv::ImageRead},
    {op::image_levels, rule_kind::plain, spv::ImageQueryLevels},
    {op::image_samples, rule_kind::plain, spv::ImageQuerySamples},
    {op::math, rule_kind::math, 0},
    {op::derivative, rule_kind::derivative, 0},
    {op::extract, rule_kind::composite, spv::CompositeExtract},
    {op::insert, rule_kind::composite, spv::CompositeInsert},
    {op::shuffle, rule_kind::composite, spv::VectorShuffle},
    {op::store, rule_kind::store, spv::Store},
    {op::array_length, rule_kind::array_length, spv::ArrayLength},
    {op::stage_load, rule_kind::stage_load, spv::Load},
    {op::stage_store, rule_kind::stage_store, spv::Store},
    {op::atomic_load, rule_kind::atomic, spv::AtomicLoad},
    {op::atomic_store, rule_kind::atomic, spv::AtomicStore},
    {op::atomic_exchange, rule_kind::atomic, spv::AtomicExchange},
    {op::atomic_compare_exchange, rule_kind::atomic, spv::AtomicCompareExchange},
    {op::atomic_iadd, rule_kind::atomic, spv::AtomicIAdd},
    {op::atomic_isub, rule_kind::atomic, spv::AtomicISub},
    {op::atomic_smin, rule_kind::atomic, spv::AtomicSMin},
    {op::atomic_umin, rule_kind::atomic, spv::AtomicUMin},
    {op::atomic_smax, rule_kind::atomic, spv::AtomicSMax},
    {op::atomic_umax, rule_kind::atomic, spv::AtomicUMax},
    {op::atomic_and, rule_kind::atomic, spv::AtomicAnd},
    {op::atomic_or, rule_kind::atomic, spv::AtomicOr},
    {op::atomic_xor, rule_kind::atomic, spv::AtomicXor},
    {op::atomic_fadd, rule_kind::atomic_fadd, spv::AtomicFAddEXT},
    {op::control_barrier, rule_kind::barrier, spv::ControlBarrier},
    {op::discard, rule_kind::discard, spv::DemoteToHelperInvocation},
    {op::sample_lod, rule_kind::sample_lod, spv::ImageSampleExplicitLod},
    {op::sample_implicit, rule_kind::plain, spv::ImageSampleImplicitLod},
    {op::image_fetch, rule_kind::image_fetch, spv::ImageFetch},
    {op::image_gather, rule_kind::image_gather, spv::ImageGather},
    {op::image_write, rule_kind::image_write, spv::ImageWrite},
    {op::image_size, rule_kind::image_size, 0},
    {op::subgroup, rule_kind::subgroup, 0},
    {op::phi, rule_kind::phi, spv::Phi},
};

/// `k_rules` by operation, since every operation of a program asks.
struct rule_table
{
    op_rule rules[isize(op::unreachable) + 1] = {};
};
constexpr rule_table k_rule_of = []
{
    auto table = rule_table();
    for (auto const& r : k_rules)
        table.rules[isize(r.code)] = r;
    return table;
}();

/// What a subgroup operation's words hold after its scope.
enum class group_form : u8
{
    plain,
    with_id,
    /// A `group_fold` before the value.
    folded,
};

struct group_rule
{
    u32 opcode = 0;
    group_form form = group_form::plain;
    u32 capability = 0;
};

/// Parallel to `subgroup`.
constexpr group_rule k_groups[] = {
    {spv::GroupNonUniformAll, group_form::plain, spv::GroupNonUniformVote},                      // all
    {spv::GroupNonUniformAny, group_form::plain, spv::GroupNonUniformVote},                      // any
    {spv::GroupNonUniformBroadcast, group_form::with_id, spv::CapGroupNonUniformBallot},         // broadcast
    {spv::GroupNonUniformBroadcastFirst, group_form::plain, spv::CapGroupNonUniformBallot},      // broadcast_first
    {spv::GroupNonUniformBallot, group_form::plain, spv::CapGroupNonUniformBallot},              // ballot
    {spv::GroupNonUniformShuffle, group_form::with_id, spv::CapGroupNonUniformShuffle},          // shuffle
    {spv::GroupNonUniformShuffleXor, group_form::with_id, spv::CapGroupNonUniformShuffle},       // shuffle_xor
    {spv::GroupNonUniformShuffleUp, group_form::with_id, spv::GroupNonUniformShuffleRelative},   // shuffle_up
    {spv::GroupNonUniformShuffleDown, group_form::with_id, spv::GroupNonUniformShuffleRelative}, // shuffle_down
    {spv::GroupNonUniformIAdd, group_form::folded, spv::GroupNonUniformArithmetic},              // iadd
    {spv::GroupNonUniformFAdd, group_form::folded, spv::GroupNonUniformArithmetic},              // fadd
    {spv::GroupNonUniformIMul, group_form::folded, spv::GroupNonUniformArithmetic},              // imul
    {spv::GroupNonUniformFMul, group_form::folded, spv::GroupNonUniformArithmetic},              // fmul
    {spv::GroupNonUniformSMin, group_form::folded, spv::GroupNonUniformArithmetic},              // smin
    {spv::GroupNonUniformUMin, group_form::folded, spv::GroupNonUniformArithmetic},              // umin
    {spv::GroupNonUniformFMin, group_form::folded, spv::GroupNonUniformArithmetic},              // fmin
    {spv::GroupNonUniformSMax, group_form::folded, spv::GroupNonUniformArithmetic},              // smax
    {spv::GroupNonUniformUMax, group_form::folded, spv::GroupNonUniformArithmetic},              // umax
    {spv::GroupNonUniformFMax, group_form::folded, spv::GroupNonUniformArithmetic},              // fmax
    {spv::GroupNonUniformBitwiseAnd, group_form::folded, spv::GroupNonUniformArithmetic},        // bit_and
    {spv::GroupNonUniformBitwiseOr, group_form::folded, spv::GroupNonUniformArithmetic},         // bit_or
    {spv::GroupNonUniformBitwiseXor, group_form::folded, spv::GroupNonUniformArithmetic},        // bit_xor
    {spv::GroupNonUniformQuadBroadcast, group_form::with_id, spv::GroupNonUniformQuad},          // quad_broadcast
    {spv::GroupNonUniformQuadSwap, group_form::with_id, spv::GroupNonUniformQuad},               // quad_swap
};
static_assert(sizeof(k_groups) / sizeof(k_groups[0]) == sizeof(char[isize(subgroup::quad_swap) + 1]));
} // namespace

void writer::ext_inst(instruction const& i, u32 id)
{
    _uses_glsl = true;
    auto const operands = _p.operands_of(i);
    head(_code, spv::ExtInst, 5 + operands.size());
    _code.push_back(id_of(i.type));
    _code.push_back(id);
    _code.push_back(_glsl);
    _code.push_back(k_glsl_numbers[i.sub]);
    for (auto const v : operands)
        _code.push_back(id_of_raw(v));
}

void writer::atomic(u32 opcode, instruction const& i, u32 id)
{
    auto const operands = _p.operands_of(i);
    auto const pointer = id_of_raw(operands[0]);
    auto const scope = literal_id(k_scope_device);
    auto const none = literal_id(0);
    auto const held = _p.at(_p.at(_p.type_of(value_ref(operands[0]))).element);
    if (held.width == 64)
        require(spv::Int64Atomics);
    if (i.code == op::atomic_store)
        return emit(_code, opcode, {pointer, scope, none, id_of_raw(operands[1])});
    if (i.code == op::atomic_load)
        return emit(_code, opcode, {id_of(i.type), id, pointer, scope, none});
    if (i.code == op::atomic_compare_exchange)
        return emit(_code, opcode,
                    {id_of(i.type), id, pointer, scope, none, none, id_of_raw(operands[1]), id_of_raw(operands[2])});
    emit(_code, opcode, {id_of(i.type), id, pointer, scope, none, id_of_raw(operands[1])});
}

void writer::group(instruction const& i, u32 id)
{
    auto const operands = _p.operands_of(i);
    auto const scope = literal_id(k_scope_subgroup);
    auto const type = id_of(i.type);
    auto const value = id_of_raw(operands[0]);
    auto const& rule = k_groups[i.sub];
    require(spv::GroupNonUniform);
    require(rule.capability);
    switch (rule.form)
    {
    case group_form::plain:
        return emit(_code, rule.opcode, {type, id, scope, value});
    case group_form::with_id:
    {
        auto const second = id_of_raw(operands[1]);
        return emit(_code, rule.opcode, {type, id, scope, value, second});
    }
    case group_form::folded:
        return emit(_code, rule.opcode, {type, id, scope, u32(i.aux) - 1, value});
    }
}

u32 writer::element_pointer(u32 element)
{
    auto const lane = _element_lanes[element];
    if (lane == 0)
        return _elements + element;
    // `SampleMask` is an array of one, the only element of a stage that stands behind one
    auto const pointer = _bound++;
    emit(_code, spv::AccessChain, {lane, pointer, _elements + element, literal_id(0)});
    return pointer;
}

void writer::frag_coord(instruction const& i, u32 id)
{
    constexpr u32 k_one_bits = 0x3f800000u;
    auto const element = _p.operands_of(i)[0];
    auto const scalar = id_of(_p.at(i.type).element);
    auto const one = _bound++;
    auto const loaded = _bound++;
    auto const w = _bound++;
    auto const reciprocal = _bound++;
    emit(_declarations, spv::Constant, {scalar, one, k_one_bits});
    emit(_code, spv::Load, {id_of(i.type), loaded, _elements + element});
    emit(_code, spv::CompositeExtract, {scalar, w, loaded, 3u});
    emit(_code, spv::FDiv, {scalar, reciprocal, one, w});
    emit(_code, spv::CompositeInsert, {id_of(i.type), id, reciprocal, loaded, 3u});
}

void writer::instruction_of(u32 index)
{
    auto const& i = _p.instructions[index];
    auto const id = _instructions + index;
    auto const operands = _p.operands_of(i);
    auto const& rule = k_rule_of.rules[isize(i.code)];
    if (i.guarantee != check::float_level::fast && rule.keeps_contraction)
        emit(_annotations, spv::Decorate, {id, spv::NoContraction});

    switch (rule.kind)
    {
    case rule_kind::plain:
        head(_code, rule.opcode, 3 + operands.size());
        _code.push_back(id_of(i.type));
        _code.push_back(id);
        for (auto const v : operands)
            _code.push_back(id_of_raw(v));
        return;
    case rule_kind::math:
        return ext_inst(i, id);
    case rule_kind::composite:
        head(_code, rule.opcode, 3 + operands.size());
        _code.push_back(id_of(i.type));
        _code.push_back(id);
        for (auto k = isize(0); k < operands.size(); ++k)
            _code.push_back(operand_role(i.code, k) == operand::value ? id_of_raw(operands[k]) : operands[k]);
        return;
    case rule_kind::store:
        return emit(_code, rule.opcode, {id_of_raw(operands[0]), id_of_raw(operands[1])});
    case rule_kind::array_length:
        return emit(_code, rule.opcode, {id_of(i.type), id, id_of_raw(operands[0]), operands[1]});
    case rule_kind::stage_load:
        if (_p.stage == check::stage::pixel && _p.stage_elements[operands[0]].system == built_in::position)
            return frag_coord(i, id);
        return emit(_code, rule.opcode, {id_of(i.type), id, element_pointer(operands[0])});
    case rule_kind::stage_store:
        return emit(_code, rule.opcode, {element_pointer(operands[0]), id_of_raw(operands[1])});
    case rule_kind::discard:
        require(spv::CapDemoteToHelperInvocation);
        return emit(_code, rule.opcode, {});
    case rule_kind::derivative:
        return emit(_code,
                    derivative(i.sub) == derivative::x   ? spv::DPdx
                    : derivative(i.sub) == derivative::y ? spv::DPdy
                                                         : spv::Fwidth,
                    {id_of(i.type), id, id_of_raw(operands[0])});
    case rule_kind::atomic:
        return atomic(rule.opcode, i, id);
    case rule_kind::atomic_fadd:
        require(_p.at(i.type).width == 64 ? spv::AtomicFloat64AddEXT : spv::AtomicFloat32AddEXT);
        require_extension("SPV_EXT_shader_atomic_float_add");
        return atomic(rule.opcode, i, id);
    case rule_kind::barrier:
    {
        auto const is_workgroup = barrier(i.sub) == barrier::workgroup;
        auto const semantics = k_semantics_acquire_release
                             | (is_workgroup ? k_semantics_workgroup : k_semantics_uniform | k_semantics_image);
        return emit(_code, rule.opcode,
                    {literal_id(k_scope_workgroup), literal_id(is_workgroup ? k_scope_workgroup : k_scope_device),
                     literal_id(semantics)});
    }
    case rule_kind::sample_lod:
        return emit(_code, rule.opcode,
                    {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), 0x2u, id_of_raw(operands[2])});
    case rule_kind::image_fetch:
        return emit(_code, rule.opcode,
                    {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), i.aux != 0 ? 0x40u : 0x2u,
                     id_of_raw(operands[2])});
    case rule_kind::image_gather:
        return emit(_code, rule.opcode,
                    {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), id_of_raw(operands[2])});
    case rule_kind::image_write:
        return emit(_code, rule.opcode, {id_of_raw(operands[0]), id_of_raw(operands[1]), id_of_raw(operands[2])});
    case rule_kind::image_size:
        require(spv::ImageQuery);
        if (i.aux != 0)
            return emit(_code, spv::ImageQuerySizeLod,
                        {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1])});
        return emit(_code, spv::ImageQuerySize, {id_of(i.type), id, id_of_raw(operands[0])});
    case rule_kind::subgroup:
        return group(i, id);
    case rule_kind::phi:
        head(_code, rule.opcode, 3 + operands.size());
        _code.push_back(id_of(i.type));
        _code.push_back(id);
        for (auto k = isize(0); k < operands.size(); k += 2)
        {
            _code.push_back(id_of_raw(operands[k]));
            _code.push_back(id_of(block_ref(operands[k + 1])));
        }
        return;
    case rule_kind::none:
        CC_UNREACHABLE("an operation with no words");
    }
}
