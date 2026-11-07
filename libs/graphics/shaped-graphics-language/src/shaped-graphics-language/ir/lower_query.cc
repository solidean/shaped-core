#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/ray_query_calls.hh>
#include <shaped-graphics-language/ir/impl/lowering.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::ir;
using namespace sgl::ir::impl;

namespace
{
/// The flag only a query that sees micromaps may state as a constant (EMIT-184).
constexpr u32 k_force_omm_2_state = 1024;

/// Parallel to `builtins::query_read`.
constexpr ray_query_value k_read_values[] = {
    ray_query_value::t,
    ray_query_value::instance_id,
    ray_query_value::instance_index,
    ray_query_value::geometry_index,
    ray_query_value::primitive_index,
    ray_query_value::barycentrics,
    ray_query_value::is_front_face,
    ray_query_value::object_to_world,
    ray_query_value::world_to_object,
    ray_query_value::object_ray_origin,
    ray_query_value::object_ray_direction,
};
} // namespace

value_ref lowering::begin_ray_query(local_id id)
{
    auto const& flags = _plan.local_flags[index_of(id)];
    auto const stated = flags.has_value() ? flags.value() : 0u;
    auto const constant = _out.sees_micromaps ? stated : stated & ~k_force_omm_2_state;
    return add(op::ray_query_begin, value_type(_e.at(id).type), {uint_constant(constant)});
}

value_ref lowering::acceleration_structure(flat_expr_id id)
{
    auto const* const bound = _e.at(id).node.try_as<flat_binding_member>();
    auto const pointer = bound != nullptr ? bound_resource(emit::impl::resource_of(_plan, bound->binding, bound->member))
                                          : bound_element(id);
    if (!is_valid(pointer))
    {
        unsupported("a trace of something that is no binding");
        return value_ref::none;
    }
    return add(op::load, _out.at(_out.type_of(pointer)).element, {pointer}, 0, nonuniform_of(pointer));
}

value_ref lowering::ray_query_read(value_ref query, ray_query_value what, bool is_committed, type_ref result)
{
    return add(op::ray_query_read, result, {query}, u8(what), is_committed ? 1 : 0);
}

value_ref lowering::ray_query_row(value_ref query, ray_query_value what, bool is_committed, value_ref row)
{
    if (!is_constant(row))
    {
        unsupported("a transform's row that is no constant");
        return value_ref::none;
    }
    auto const three = vector_type(_float, 3);
    auto const matrix = intern({.kind = type_kind::matrix, .count = 4, .element = three});
    auto const whole = ray_query_read(query, what, is_committed, matrix);
    u32 lanes[4];
    auto const at = _out.constants[index_of(row)].low;
    for (auto column = u32(0); column < 4; ++column)
        lanes[column] = u32(extracted(extracted(whole, three, column), _float, at));
    return add(op::construct, vector_type(_float, 4), lanes);
}

value_ref lowering::ray_query_call(flat_call const& c, builtins::function_record const& record)
{
    using builtins::query_op;
    using builtins::query_read;
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto const query = expr(arguments[0]);
    if (!_failure.empty())
        return value_ref::none;
    auto const what = query_op(record.write.data);
    if (u32(what) >= u32(query_op::read_base))
    {
        auto const code = u32(what) - u32(query_op::read_base);
        auto const read = query_read(code / 2);
        auto const is_committed = code % 2 == 1;
        auto const value = k_read_values[isize(read)];
        if (read == query_read::object_to_world_row || read == query_read::world_to_object_row)
            return ray_query_row(query, value, is_committed, expr(arguments[1]));
        auto const type = read == query_read::t                    ? _float
                        : read == query_read::barycentrics         ? vector_type(_float, 2)
                        : read == query_read::is_front_face        ? _bool
                        : read == query_read::object_ray_origin    ? vector_type(_float, 3)
                        : read == query_read::object_ray_direction ? vector_type(_float, 3)
                                                                   : _int;
        return ray_query_read(query, value, is_committed, type);
    }
    switch (what)
    {
    case query_op::trace:
    {
        auto const world = acceleration_structure(arguments[1]);
        u32 operands[8];
        operands[0] = u32(query);
        operands[1] = u32(world);
        for (auto i = isize(2); i < 6; ++i)
            operands[i] = u32(expr(arguments[i]));
        operands[6] = u32(converted(expr(arguments[6]), _int, _uint));
        operands[7] = u32(converted(expr(arguments[7]), _int, _uint));
        return add(op::ray_query_trace, type_ref::none, operands);
    }
    case query_op::proceed:
        return add(op::ray_query_proceed, _bool, {query});
    case query_op::candidate_is_triangle:
    {
        auto const kind = ray_query_read(query, ray_query_value::candidate_kind, false, _int);
        return add(op::ieq, _bool, {kind, int_constant(0)});
    }
    case query_op::commit_triangle:
        return add(op::ray_query_commit_triangle, type_ref::none, {query});
    case query_op::abort:
        return add(op::ray_query_abort, type_ref::none, {query});
    case query_op::committed_kind:
        return ray_query_read(query, ray_query_value::committed_kind, true, _int);
    case query_op::commit_procedural:
        return add(op::ray_query_commit_procedural, type_ref::none, {query, expr(arguments[1])});
    case query_op::candidate_is_opaque_box:
        return ray_query_read(query, ray_query_value::candidate_box_is_opaque, false, _bool);
    default:
        unsupported(cc::format("the query step {}", record.name));
        return value_ref::none;
    }
}
