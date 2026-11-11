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
    stream,
    set_mesh_counts,
    write_primitive,
    derivative,
    sample_lod,
    sample_implicit,
    sample_bias,
    sample_grad,
    sample_compare_zero,
    image_fetch,
    image_gather,
    image_write,
    image_size,
    subgroup,
    ray_query,
    ray_stage,
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
    {op::texel_pointer, rule_kind::plain, spv::ImageTexelPointer},
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
    {op::emit_vertex, rule_kind::stream, spv::EmitVertex},
    {op::end_strip, rule_kind::stream, spv::EndPrimitive},
    {op::set_mesh_counts, rule_kind::set_mesh_counts, spv::SetMeshOutputsEXT},
    {op::write_primitive, rule_kind::write_primitive, spv::Store},
    {op::sample_lod, rule_kind::sample_lod, spv::ImageSampleExplicitLod},
    {op::sample_implicit, rule_kind::sample_implicit, spv::ImageSampleImplicitLod},
    {op::sample_bias, rule_kind::sample_bias, spv::ImageSampleImplicitLod},
    {op::sample_grad, rule_kind::sample_grad, spv::ImageSampleExplicitLod},
    {op::sample_compare, rule_kind::plain, spv::ImageSampleDrefImplicitLod},
    {op::sample_compare_zero, rule_kind::sample_compare_zero, spv::ImageSampleDrefExplicitLod},
    {op::image_fetch, rule_kind::image_fetch, spv::ImageFetch},
    {op::image_gather, rule_kind::image_gather, spv::ImageGather},
    {op::image_gather_compare, rule_kind::image_gather, spv::ImageDrefGather},
    {op::image_write, rule_kind::image_write, spv::ImageWrite},
    {op::image_size, rule_kind::image_size, 0},
    {op::subgroup, rule_kind::subgroup, 0},
    {op::trace_ray, rule_kind::ray_stage, 0},
    {op::execute_callable, rule_kind::ray_stage, 0},
    {op::report_hit, rule_kind::ray_stage, 0},
    {op::hit_object_trace, rule_kind::ray_stage, 0},
    {op::trace_reorder_execute, rule_kind::ray_stage, 0},
    {op::hit_object_reorder, rule_kind::ray_stage, 0},
    {op::reorder_thread, rule_kind::ray_stage, 0},
    {op::hit_object_invoke, rule_kind::ray_stage, 0},
    {op::hit_object_read, rule_kind::ray_stage, 0},
    {op::acceleration_from_address, rule_kind::plain, spv::ConvertUToAccelerationStructureKHR},
    {op::ray_query_begin, rule_kind::ray_query, 0},
    {op::ray_query_trace, rule_kind::ray_query, 0},
    {op::ray_query_proceed, rule_kind::ray_query, 0},
    {op::ray_query_commit_triangle, rule_kind::ray_query, 0},
    {op::ray_query_commit_procedural, rule_kind::ray_query, 0},
    {op::ray_query_abort, rule_kind::ray_query, 0},
    {op::ray_query_read, rule_kind::ray_query, 0},
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
    auto const& pointed = _p.at(_p.type_of(value_ref(operands[0])));
    // workgroup memory is one workgroup's, which is the scope DXC states of it too
    auto const scope = literal_id(pointed.space == storage::workgroup ? k_scope_workgroup : k_scope_device);
    auto const none = literal_id(0);
    auto const held = _p.at(pointed.element);
    if (held.width == 64)
        require(spv::Int64Atomics);
    // EMIT-258: a 64-bit word of workgroup memory is stored by an exchange and read by an `or` of 0, as DXC writes them
    auto const is_shared_wide = held.width == 64 && pointed.space == storage::workgroup;
    if (i.code == op::atomic_store && is_shared_wide)
        return emit(_code, spv::AtomicExchange,
                    {id_of(pointed.element), _bound++, pointer, scope, none, id_of_raw(operands[1])});
    if (i.code == op::atomic_load && is_shared_wide)
    {
        auto const zero = _bound++;
        emit(_declarations, spv::ConstantNull, {id_of(pointed.element), zero});
        return emit(_code, spv::AtomicOr, {id_of(i.type), id, pointer, scope, none, zero});
    }
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

void writer::nonuniform(instruction const& i, u32 id)
{
    require(spv::ShaderNonUniform);
    if (i.code == op::combine_sampler)
        require(spv::SampledImageArrayNonUniformIndexing);
    auto const mark = [&](u32 marked)
    {
        for (auto const known : _nonuniform)
            if (known == marked)
                return;
        _nonuniform.push_back(marked);
        emit(_annotations, spv::Decorate, {marked, spv::NonUniform});
    };
    mark(id);
    auto const index = value_ref(_p.operands_of(i)[i.code == op::access_chain ? 1 : 0]);
    if (i.code == op::access_chain && !is_constant(index))
        mark(id_of(index));
}

u32 writer::element_pointer(u32 element, u32 index)
{
    auto const lane = _element_lanes[element];
    if (lane == 0)
        return _elements + element;
    // an element behind an array: per control point or vertex, `SampleMask` of one, or a tessellation factor
    auto const& e = _p.stage_elements[element];
    auto const pointer = _bound++;
    auto const at = e.array_size > 0 ? index : literal_id(e.system == built_in::sample_mask ? 0 : e.semantic_index);
    emit(_code, spv::AccessChain, {lane, pointer, _element_variables[element], at});
    return pointer;
}

void writer::tess_coord(instruction const& i, u32 id)
{
    auto const element = _p.operands_of(i)[0];
    auto three = type_ref::none;
    for (auto t = isize(0); t < _p.types.size(); ++t)
        if (_p.types[t].kind == type_kind::vector && _p.types[t].count == 3
            && _p.types[t].element == _p.at(i.type).element)
            three = type_ref(t);
    if (i.type == three)
        return emit(_code, spv::Load, {id_of(i.type), id, _elements + element});
    auto const loaded = _bound++;
    emit(_code, spv::Load, {id_of(three), loaded, _elements + element});
    emit(_code, spv::VectorShuffle, {id_of(i.type), id, loaded, loaded, 0u, 1u});
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

void writer::hit_object(instruction const& i, u32 id)
{
    /// Parallel to `hit_object_value`.
    constexpr u32 k_reads[] = {
        spv::HitObjectIsHitEXT,
        spv::HitObjectIsMissEXT,
        spv::HitObjectGetRayTMaxEXT,
        spv::HitObjectGetInstanceCustomIndexEXT,
        spv::HitObjectGetInstanceIdEXT,
        spv::HitObjectGetGeometryIndexEXT,
        spv::HitObjectGetPrimitiveIndexEXT,
    };
    auto const operands = _p.operands_of(i);
    auto const at = [&](isize k) { return id_of_raw(operands[k]); };
    auto const trace = [&](u32 opcode)
    {
        head(_code, opcode, 2 + operands.size());
        _code.push_back(id);
        for (auto k = isize(0); k < operands.size(); ++k)
            _code.push_back(at(k));
    };
    switch (i.code)
    {
    case op::hit_object_trace:
        return trace(spv::HitObjectTraceRayEXT);
    case op::trace_reorder_execute:
        return trace(spv::TraceReorderExecuteEXT);
    case op::hit_object_reorder:
        if (operands.size() == 1)
            return emit(_code, spv::ReorderThreadWithHitObjectEXT, {at(0)});
        return emit(_code, spv::ReorderThreadWithHitObjectEXT, {at(0), at(1), at(2)});
    case op::reorder_thread:
        require(spv::ShaderInvocationReorderEXT);
        require_extension("SPV_EXT_shader_invocation_reorder");
        return emit(_code, spv::ReorderThreadWithHintEXT, {at(0), at(1)});
    case op::hit_object_invoke:
        return emit(_code, spv::HitObjectExecuteShaderEXT, {at(0), at(1)});
    default:
        return emit(_code, k_reads[i.sub], {id_of(i.type), id, at(0)});
    }
}

void writer::ray_query(instruction const& i, u32 id)
{
    /// Parallel to `ray_query_value`.
    constexpr u32 k_reads[] = {
        spv::RayQueryGetIntersectionTypeKHR,
        spv::RayQueryGetIntersectionTypeKHR,
        spv::RayQueryGetIntersectionCandidateAABBOpaqueKHR,
        spv::RayQueryGetIntersectionTKHR,
        spv::RayQueryGetIntersectionInstanceCustomIndexKHR,
        spv::RayQueryGetIntersectionInstanceIdKHR,
        spv::RayQueryGetIntersectionGeometryIndexKHR,
        spv::RayQueryGetIntersectionPrimitiveIndexKHR,
        spv::RayQueryGetIntersectionBarycentricsKHR,
        spv::RayQueryGetIntersectionFrontFaceKHR,
        spv::RayQueryGetIntersectionObjectToWorldKHR,
        spv::RayQueryGetIntersectionWorldToObjectKHR,
        spv::RayQueryGetIntersectionObjectRayOriginKHR,
        spv::RayQueryGetIntersectionObjectRayDirectionKHR,
        spv::RayQueryGetIntersectionClusterIdNV,
    };
    auto const operands = _p.operands_of(i);
    auto const at = [&](isize k) { return id_of_raw(operands[k]); };
    switch (i.code)
    {
    case op::ray_query_trace:
        return emit(_code, spv::RayQueryInitializeKHR, {at(0), at(1), at(6), at(7), at(2), at(4), at(3), at(5)});
    case op::ray_query_proceed:
        return emit(_code, spv::RayQueryProceedKHR, {id_of(i.type), id, at(0)});
    case op::ray_query_commit_triangle:
        return emit(_code, spv::RayQueryConfirmIntersectionKHR, {at(0)});
    case op::ray_query_commit_procedural:
        return emit(_code, spv::RayQueryGenerateIntersectionKHR, {at(0), at(1)});
    case op::ray_query_abort:
        return emit(_code, spv::RayQueryTerminateKHR, {at(0)});
    case op::ray_query_read:
    {
        auto const opcode = k_reads[i.sub];
        if (ray_query_value(i.sub) == ray_query_value::cluster_id)
        {
            require(spv::RayTracingClusterAccelerationStructureNV);
            require_extension("SPV_NV_cluster_acceleration_structure");
        }
        if (ray_query_value(i.sub) == ray_query_value::candidate_box_is_opaque)
            return emit(_code, opcode, {id_of(i.type), id, at(0)});
        return emit(_code, opcode, {id_of(i.type), id, at(0), literal_id(i.aux)});
    }
    default:
        return; // ray_query_begin: the variable `write_function` declared at the top of the body
    }
}

void writer::find_scalings()
{
    auto const count = _p.instructions.size();
    _splat_operand = cc::vector<u8>::create_filled(count, k_no_splat);
    _unwritten = cc::vector<bool>::create_filled(count, false);
    // a splat is one scalar operation in every lane of a float vector; one of constants is a constant of the module
    auto const is_splat = [&](u32 raw)
    {
        auto const v = value_ref(raw);
        if (!is_operation(v))
            return false;
        auto const& c = _p.instructions[index_of(v)];
        auto const& t = _p.at(c.type);
        if (c.code != op::construct || t.kind != type_kind::vector || c.count != t.count || c.count < 2
            || _p.at(t.element).kind != type_kind::float_)
            return false;
        auto const lanes = _p.operands_of(c);
        if (!is_operation(value_ref(lanes[0])))
            return false;
        for (auto const lane : lanes)
            if (lane != lanes[0])
                return false;
        return true;
    };
    for (auto k = isize(0); k < count; ++k)
    {
        auto const& i = _p.instructions[k];
        if (i.code != op::fmul || _p.at(i.type).kind != type_kind::vector)
            continue;
        auto const operands = _p.operands_of(i);
        if (is_splat(operands[1]))
            _splat_operand[k] = 1;
        else if (is_splat(operands[0]))
            _splat_operand[k] = 0;
    }
    // a splat any other operation reads is written still
    auto read_otherwise = cc::vector<bool>::create_filled(count, false);
    for (auto k = isize(0); k < count; ++k)
    {
        auto const& i = _p.instructions[k];
        auto const operands = _p.operands_of(i);
        for (auto o = isize(0); o < operands.size(); ++o)
            if (operand_role(i.code, o) == operand::value && is_operation(value_ref(operands[o]))
                && o != isize(_splat_operand[k]))
                read_otherwise[index_of(value_ref(operands[o]))] = true;
    }
    for (auto k = isize(0); k < count; ++k)
        if (_splat_operand[k] != k_no_splat)
        {
            auto const splat = index_of(value_ref(_p.operands_of(_p.instructions[k])[_splat_operand[k]]));
            _unwritten[splat] = !read_otherwise[splat];
        }
}

void writer::instruction_of(u32 index)
{
    if (_unwritten[index])
        return;
    auto const& i = _p.instructions[index];
    auto const id = _instructions + index;
    auto const operands = _p.operands_of(i);
    auto const& rule = k_rule_of.rules[isize(i.code)];
    if (i.guarantee != check::float_level::fast && rule.keeps_contraction)
        emit(_annotations, spv::Decorate, {id, spv::NoContraction});
    if (i.aux == k_ir_nonuniform && (i.code == op::access_chain || i.code == op::load || i.code == op::combine_sampler))
        nonuniform(i, id);

    switch (rule.kind)
    {
    case rule_kind::plain:
    {
        // a vector of constants is a constant of the module, which a driver need not build per invocation
        auto is_constant_composite = i.code == op::construct;
        for (auto const v : operands)
            is_constant_composite = is_constant_composite && is_constant(value_ref(v))
                                 && !_p.constants[index_of(value_ref(v))].is_undefined;
        if (is_constant_composite)
        {
            head(_declarations, spv::ConstantComposite, 3 + operands.size());
            _declarations.push_back(id_of(i.type));
            _declarations.push_back(id);
            for (auto const v : operands)
                _declarations.push_back(id_of_raw(v));
            return;
        }
        if (_splat_operand[index] != k_no_splat)
        {
            auto const splat = _p.instructions[index_of(value_ref(operands[_splat_operand[index]]))];
            emit(_code, spv::VectorTimesScalar,
                 {id_of(i.type), id, id_of_raw(operands[1 - _splat_operand[index]]), id_of_raw(_p.operands_of(splat)[0])});
            return;
        }
        head(_code, rule.opcode, 3 + operands.size());
        _code.push_back(id_of(i.type));
        _code.push_back(id);
        for (auto const v : operands)
            _code.push_back(id_of_raw(v));
        return;
    }
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
    {
        if (_p.stage == check::stage::pixel && _p.stage_elements[operands[0]].system == built_in::position)
            return frag_coord(i, id);
        if (_p.stage_elements[operands[0]].system == built_in::domain_location)
            return tess_coord(i, id);
        auto const pointer = element_pointer(operands[0], operands.size() > 1 ? id_of_raw(operands[1]) : 0);
        return emit(_code, rule.opcode, {id_of(i.type), id, pointer});
    }
    case rule_kind::stage_store:
    {
        auto const pointer = element_pointer(operands[0], operands.size() > 2 ? id_of_raw(operands[2]) : 0);
        return emit(_code, rule.opcode, {pointer, id_of_raw(operands[1])});
    }
    case rule_kind::stream:
        return emit(_code, rule.opcode, {});
    case rule_kind::set_mesh_counts:
        return emit(_code, rule.opcode, {id_of_raw(operands[0]), id_of_raw(operands[1])});
    case rule_kind::write_primitive:
    {
        auto const pointer = _bound++;
        emit(_code, spv::AccessChain, {_primitive_lane, pointer, _primitive_indices, id_of_raw(operands[0])});
        return emit(_code, rule.opcode, {pointer, id_of_raw(operands[1])});
    }
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
    case rule_kind::sample_implicit:
        if (operands.size() > 2)
            return emit(_code, rule.opcode,
                        {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), k_image_const_offset,
                         constant_composite(value_ref(operands[2]))});
        return emit(_code, rule.opcode, {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1])});
    case rule_kind::sample_bias:
        return emit(_code, rule.opcode,
                    {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), 0x1u, id_of_raw(operands[2])});
    case rule_kind::sample_grad:
        return emit(_code, rule.opcode,
                    {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), 0x4u, id_of_raw(operands[2]),
                     id_of_raw(operands[3])});
    case rule_kind::sample_compare_zero:
        return emit(_code, rule.opcode,
                    {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), id_of_raw(operands[2]), 0x2u,
                     id_of_raw(operands[3])});
    case rule_kind::image_fetch:
        return emit(_code, rule.opcode,
                    {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), i.aux != 0 ? 0x40u : 0x2u,
                     id_of_raw(operands[2])});
    case rule_kind::image_gather:
        if (operands.size() > 3)
            return emit(_code, rule.opcode,
                        {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), id_of_raw(operands[2]),
                         k_image_const_offset, constant_composite(value_ref(operands[3]))});
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
    case rule_kind::ray_query:
        return ray_query(i, id);
    case rule_kind::ray_stage:
    {
        auto const at = [&](isize k) { return id_of_raw(operands[k]); };
        if (i.code == op::execute_callable)
            return emit(_code, spv::ExecuteCallableKHR, {at(0), at(1)});
        if (i.code == op::report_hit)
            return emit(_code, spv::ReportIntersectionKHR, {id_of(i.type), id, at(0), at(1)});
        if (i.code == op::trace_ray)
            return emit(_code, spv::TraceRayKHR,
                        {at(0), at(1), at(2), at(3), at(4), at(5), at(6), at(7), at(8), at(9), at(10)});
        return hit_object(i, id);
    }
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
