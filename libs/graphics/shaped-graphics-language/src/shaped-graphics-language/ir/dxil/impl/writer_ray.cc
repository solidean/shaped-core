#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ir/dxil/impl/dx_ops.hh>
#include <shaped-graphics-language/ir/dxil/impl/writer.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil;
using namespace sgl::ir::dxil::impl;

// What a ray-tracing stage adds: a library's one function, its pointers, and what it reads of the ray.

namespace
{
/// How a `built_in` of a ray-tracing stage reads.
struct ray_read_rule
{
    ir::built_in input = ir::built_in::none;
    dx_op op = dx_op::instance_id;
};
constexpr ray_read_rule k_ray_reads[] = {
    {ir::built_in::launch_id, dx_op::dispatch_rays_index},
    {ir::built_in::launch_size, dx_op::dispatch_rays_dimensions},
    {ir::built_in::world_ray_origin, dx_op::world_ray_origin},
    {ir::built_in::world_ray_direction, dx_op::world_ray_direction},
    {ir::built_in::object_ray_origin, dx_op::object_ray_origin},
    {ir::built_in::object_ray_direction, dx_op::object_ray_direction},
    {ir::built_in::ray_t_min, dx_op::ray_t_min},
    {ir::built_in::ray_t_current, dx_op::ray_t_current},
    {ir::built_in::ray_instance_id, dx_op::instance_id},
    {ir::built_in::ray_instance_index, dx_op::instance_index},
    {ir::built_in::ray_geometry_index, dx_op::geometry_index},
    {ir::built_in::ray_primitive_index, dx_op::primitive_index},
    {ir::built_in::hit_kind, dx_op::hit_kind},
    {ir::built_in::object_to_world, dx_op::object_to_world},
    {ir::built_in::world_to_object, dx_op::world_to_object},
};

[[nodiscard]] constexpr bool is_ray_space(storage s)
{
    return s == storage::ray_input || s == storage::hit_attribute || s == storage::ray_output
        || s == storage::callable_output;
}
} // namespace

bool writer::is_library() const
{
    return _p.stage >= check::stage::raygen;
}

ll_type writer::entry_function_type()
{
    auto const is_hit = _p.stage == check::stage::closest_hit || _p.stage == check::stage::any_hit;
    _payload_roots.resize_to_filled(_p.globals.size(), {});
    auto parameters = cc::vector<ll_type>();
    auto input = -1;
    auto attributes = -1;
    for (auto g = isize(0); g < _p.globals.size(); ++g)
    {
        auto const space = _p.at(_p.globals[g].type).space;
        if (space == storage::ray_input)
            input = i32(g);
        if (space == storage::hit_attribute)
            attributes = i32(g);
    }
    if (input >= 0)
        parameters.push_back(_m.pointer(root_type(u32(input))));
    if (is_hit)
    {
        if (input < 0)
            unsupported("a hit stage that takes no payload, which only metal builds");
        if (attributes >= 0)
            parameters.push_back(_m.pointer(root_type(u32(attributes))));
        else
            parameters.push_back(_m.pointer(triangle_attributes_type()));
    }
    _m.set_arguments(parameters);
    return _m.function(_m.void_type(), parameters);
}

ll_type writer::triangle_attributes_type()
{
    ll_type const members[] = {_m.array(_m.float_type(), 2)};
    return _m.structure("struct.BuiltInTriangleIntersectionAttributes", members);
}

ll_type writer::root_type(u32 global)
{
    auto const pointee = _p.at(_p.globals[global].type).element;
    if (_p.at(pointee).kind == type_kind::structure)
        return payload_type(pointee);
    // a triangle's barycentrics are the one attribute that is no struct of the program's
    _payload_roots[global].is_wrapped = true;
    return triangle_attributes_type();
}

void writer::declare_ray_roots()
{
    auto const has_attributes_parameter = _p.stage == check::stage::closest_hit || _p.stage == check::stage::any_hit;
    for (auto g = isize(0); g < _p.globals.size(); ++g)
    {
        auto const& pointer = _p.at(_p.globals[g].type);
        if (!is_ray_space(pointer.space))
            continue;
        auto& root = _payload_roots[g];
        auto alignment = u32(1);
        auto const bytes = payload_layout(pointer.element, alignment);
        root.type = root_type(u32(g));
        switch (pointer.space)
        {
        case storage::ray_input:
            root.pointer = _m.argument(0);
            _summary.payload_bytes = bytes;
            break;
        case storage::hit_attribute:
            root.pointer = has_attributes_parameter ? _m.argument(1) : _m.alloca_of(root.type, alignment);
            _summary.attribute_bytes = bytes;
            break;
        default:
            root.pointer = _m.alloca_of(root.type, alignment);
            break;
        }
    }
    if (has_attributes_parameter && _summary.attribute_bytes == 0)
        _summary.attribute_bytes = 8;
}

void writer::ray_element(u32 index, stage_element const& e, instruction const& i)
{
    ray_read_rule const* found = nullptr;
    for (auto const& r : k_ray_reads)
        if (r.input == e.system)
            found = &r;
    if (found == nullptr)
        return unsupported(cc::format("the stage input {} on DXIL", int(e.system)));
    auto const number = _m.constant_i32(k_dx_ops[isize(found->op)].number);
    auto const& info = _p.at(i.type);
    // brought by raytracing tier 1.1, as a query was
    _uses.ray_query = _uses.ray_query || e.system == ir::built_in::ray_geometry_index;
    _scratch.clear();
    if (info.kind == type_kind::matrix)
    {
        for (auto column = u32(0); column < info.count; ++column)
            for (auto row = u32(0); row < 3; ++row)
            {
                ll_value const args[] = {number, _m.constant_i32(row), _m.constant_i8(column)};
                _scratch.push_back(call(found->op, _m.float_type(), args));
            }
        return define(index);
    }
    if (info.kind == type_kind::vector)
    {
        auto const overload = scalar_type(info.element);
        for (auto lane = u32(0); lane < info.count; ++lane)
        {
            ll_value const args[] = {number, _m.constant_i8(lane)};
            _scratch.push_back(call(found->op, overload, args));
        }
        return define(index);
    }
    ll_value const args[] = {number};
    define_one(index, call(found->op, scalar_type(i.type), args));
}

void writer::hit_object_op(u32 index, instruction const& i)
{
    /// Parallel to `hit_object_value`.
    constexpr dx_op k_reads[] = {
        dx_op::hit_object_is_hit,          dx_op::hit_object_is_miss,        dx_op::hit_object_t,
        dx_op::hit_object_instance_id,     dx_op::hit_object_instance_index, dx_op::hit_object_geometry_index,
        dx_op::hit_object_primitive_index,
    };
    _uses.reorders = true;
    auto const operands = _p.operands_of(i);
    auto const at = [&](isize k) { return leaf_at(value_ref(operands[k])); };
    auto const number = [&](dx_op code) { return _m.constant_i32(k_dx_ops[isize(code)].number); };
    auto const reorder = [&](ll_value hit, ll_value hint, ll_value bits)
    {
        ll_value const args[] = {number(dx_op::maybe_reorder_thread), hit, hint, bits};
        (void)call(dx_op::maybe_reorder_thread, _m.void_type(), args);
    };
    auto const invoke = [&](ll_value hit, payload_root const& root)
    {
        ll_value const args[] = {number(dx_op::hit_object_invoke), hit, root.pointer};
        (void)call(dx_op::hit_object_invoke, root.type, args);
    };
    auto const traced = [&](payload_root const& root)
    {
        auto const origin = value_ref(operands[6]);
        auto const direction = value_ref(operands[8]);
        ll_value const args[] = {number(dx_op::hit_object_trace_ray),
                                 at(0),
                                 at(1),
                                 at(2),
                                 at(3),
                                 at(4),
                                 at(5),
                                 leaf_at(origin, 0),
                                 leaf_at(origin, 1),
                                 leaf_at(origin, 2),
                                 at(7),
                                 leaf_at(direction, 0),
                                 leaf_at(direction, 1),
                                 leaf_at(direction, 2),
                                 at(9),
                                 root.pointer};
        return call(dx_op::hit_object_trace_ray, root.type, args);
    };
    switch (i.code)
    {
    case op::hit_object_trace:
        return define_one(index, traced(_payload_roots[index_of(value_ref(operands[10]))]));
    case op::trace_reorder_execute:
    {
        auto const& root = _payload_roots[index_of(value_ref(operands[10]))];
        auto const hit = traced(root);
        if (operands.size() > 11)
            reorder(hit, at(11), at(12));
        else
            reorder(hit, _m.constant_undef(_m.int_type()), _m.constant_i32(0));
        invoke(hit, root);
        return define_one(index, hit);
    }
    case op::hit_object_reorder:
        if (operands.size() > 1)
            return reorder(at(0), at(1), at(2));
        return reorder(at(0), _m.constant_undef(_m.int_type()), _m.constant_i32(0));
    case op::reorder_thread:
    {
        ll_value const args[] = {number(dx_op::hit_object_make_nop)};
        return reorder(call(dx_op::hit_object_make_nop, _m.void_type(), args), at(0), at(1));
    }
    case op::hit_object_invoke:
        return invoke(at(0), _payload_roots[index_of(value_ref(operands[1]))]);
    default:
    {
        auto const code = k_reads[i.sub];
        ll_value const args[] = {number(code), at(0)};
        return define_one(index, call(code, scalar_type(i.type), args));
    }
    }
}

void writer::ray_stage_op(u32 index, instruction const& i)
{
    if (i.code != op::trace_ray && i.code != op::execute_callable && i.code != op::report_hit)
        return hit_object_op(index, i);
    auto const operands = _p.operands_of(i);
    auto const at = [&](isize k) { return leaf_at(value_ref(operands[k])); };
    auto const number = [&](dx_op code) { return _m.constant_i32(k_dx_ops[isize(code)].number); };
    auto const& root = _payload_roots[index_of(value_ref(operands[operands.size() - 1]))];
    switch (i.code)
    {
    case op::trace_ray:
    {
        auto const origin = value_ref(operands[6]);
        auto const direction = value_ref(operands[8]);
        ll_value const args[] = {number(dx_op::trace_ray),
                                 at(0),
                                 at(1),
                                 at(2),
                                 at(3),
                                 at(4),
                                 at(5),
                                 leaf_at(origin, 0),
                                 leaf_at(origin, 1),
                                 leaf_at(origin, 2),
                                 at(7),
                                 leaf_at(direction, 0),
                                 leaf_at(direction, 1),
                                 leaf_at(direction, 2),
                                 at(9),
                                 root.pointer};
        (void)call(dx_op::trace_ray, root.type, args);
        return;
    }
    case op::execute_callable:
    {
        ll_value const args[] = {number(dx_op::call_shader), at(0), root.pointer};
        (void)call(dx_op::call_shader, root.type, args);
        return;
    }
    default:
    {
        ll_value const args[] = {number(dx_op::report_hit), at(0), at(1), root.pointer};
        return define_one(index, call(dx_op::report_hit, root.type, args));
    }
    }
}

void writer::end_of_hit(dx_op code)
{
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(code)].number)};
    (void)call(code, _m.void_type(), args);
    _m.unreachable();
}
