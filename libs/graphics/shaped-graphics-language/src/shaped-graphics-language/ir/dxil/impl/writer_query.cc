#include <shaped-graphics-language/ir/dxil/impl/dx_ops.hh>
#include <shaped-graphics-language/ir/dxil/impl/writer.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil;
using namespace sgl::ir::dxil::impl;

namespace
{
/// How a read of a query is spelled, per `ray_query_value`.
struct query_read_rule
{
    dx_op candidate;
    dx_op committed;
};
constexpr query_read_rule k_query_reads[] = {
    {dx_op::ray_query_committed_status, dx_op::ray_query_committed_status},
    {dx_op::ray_query_candidate_type, dx_op::ray_query_candidate_type},
    {dx_op::ray_query_candidate_box_non_opaque, dx_op::ray_query_candidate_box_non_opaque},
    {dx_op::ray_query_candidate_t, dx_op::ray_query_committed_t},
    {dx_op::ray_query_candidate_instance_id, dx_op::ray_query_committed_instance_id},
    {dx_op::ray_query_candidate_instance_index, dx_op::ray_query_committed_instance_index},
    {dx_op::ray_query_candidate_geometry, dx_op::ray_query_committed_geometry},
    {dx_op::ray_query_candidate_primitive, dx_op::ray_query_committed_primitive},
    {dx_op::ray_query_candidate_barycentrics, dx_op::ray_query_committed_barycentrics},
    {dx_op::ray_query_candidate_front_face, dx_op::ray_query_committed_front_face},
    {dx_op::ray_query_candidate_object_to_world, dx_op::ray_query_committed_object_to_world},
    {dx_op::ray_query_candidate_world_to_object, dx_op::ray_query_committed_world_to_object},
    {dx_op::ray_query_candidate_object_origin, dx_op::ray_query_committed_object_origin},
    {dx_op::ray_query_candidate_object_direction, dx_op::ray_query_committed_object_direction},
    // cluster_id: none before Shader Model 6.10, which `ray_query_read` refuses ahead of reading this
    {dx_op::ray_query_committed_status, dx_op::ray_query_committed_status},
};
} // namespace

void writer::ray_query_read(u32 index, instruction const& i)
{
    auto const what = ray_query_value(i.sub);
    if (what == ray_query_value::cluster_id)
        return unsupported("a hit's cluster_id, which DXIL has from Shader Model 6.10");
    auto const& rule = k_query_reads[i.sub];
    auto const code = i.aux != 0 ? rule.committed : rule.candidate;
    auto const number = _m.constant_i32(k_dx_ops[isize(code)].number);
    auto const query = leaf_at(value_ref(_p.operands_of(i)[0]));
    auto const& info = _p.at(i.type);
    _scratch.clear();
    if (info.kind == type_kind::matrix)
    {
        // DXIL asks by row and column, and a program's matrix stands in columns of three
        auto const overload = _m.float_type();
        for (auto column = u32(0); column < info.count; ++column)
            for (auto row = u32(0); row < 3; ++row)
            {
                ll_value const args[] = {number, query, _m.constant_i32(row), _m.constant_i8(column)};
                _scratch.push_back(call(code, overload, args));
            }
        return define(index);
    }
    if (info.kind == type_kind::vector)
    {
        auto const overload = _m.float_type();
        for (auto lane = u32(0); lane < info.count; ++lane)
        {
            ll_value const args[] = {number, query, _m.constant_i8(lane)};
            _scratch.push_back(call(code, overload, args));
        }
        return define(index);
    }
    ll_value const args[] = {number, query};
    auto const value = call(code, scalar_type(i.type), args);
    if (what == ray_query_value::candidate_box_is_opaque)
        return define_one(index, _m.binary(binary_op::bit_xor, value, _m.constant_bool(true)));
    define_one(index, value);
}

void writer::ray_query_op(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const at = [&](isize k) { return leaf_at(value_ref(operands[k])); };
    auto const number = [&](dx_op code) { return _m.constant_i32(k_dx_ops[isize(code)].number); };
    switch (i.code)
    {
    case op::ray_query_begin:
    {
        _uses.ray_query = true;
        if (_p.sees_micromaps)
        {
            constexpr u32 k_allow_opacity_micromaps = 1;
            ll_value const args[]
                = {number(dx_op::allocate_ray_query_2), at(0), _m.constant_i32(k_allow_opacity_micromaps)};
            return define_one(index, call(dx_op::allocate_ray_query_2, _m.void_type(), args));
        }
        ll_value const args[] = {number(dx_op::allocate_ray_query), at(0)};
        return define_one(index, call(dx_op::allocate_ray_query, _m.void_type(), args));
    }
    case op::ray_query_trace:
    {
        auto const origin = value_ref(operands[2]);
        auto const direction = value_ref(operands[3]);
        ll_value const args[] = {number(dx_op::ray_query_trace_inline),
                                 at(0),
                                 at(1),
                                 at(6),
                                 at(7),
                                 leaf_at(origin, 0),
                                 leaf_at(origin, 1),
                                 leaf_at(origin, 2),
                                 at(4),
                                 leaf_at(direction, 0),
                                 leaf_at(direction, 1),
                                 leaf_at(direction, 2),
                                 at(5)};
        (void)call(dx_op::ray_query_trace_inline, _m.void_type(), args);
        return;
    }
    case op::ray_query_proceed:
    {
        ll_value const args[] = {number(dx_op::ray_query_proceed), at(0)};
        return define_one(index, call(dx_op::ray_query_proceed, _m.bool_type(), args));
    }
    case op::ray_query_commit_triangle:
    {
        ll_value const args[] = {number(dx_op::ray_query_commit_triangle), at(0)};
        (void)call(dx_op::ray_query_commit_triangle, _m.void_type(), args);
        return;
    }
    case op::ray_query_commit_procedural:
    {
        ll_value const args[] = {number(dx_op::ray_query_commit_procedural), at(0), at(1)};
        (void)call(dx_op::ray_query_commit_procedural, _m.void_type(), args);
        return;
    }
    case op::ray_query_abort:
    {
        ll_value const args[] = {number(dx_op::ray_query_abort), at(0)};
        (void)call(dx_op::ray_query_abort, _m.void_type(), args);
        return;
    }
    default:
        return ray_query_read(index, i);
    }
}
