#include <clean-core/string/format.hh>
#include <shaped-graphics-language/emit/impl/payload_access.hh>
#include <shaped-graphics-language/ir/impl/lowering.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::ir;
using namespace sgl::ir::impl;

namespace
{
/// `HIT_KIND_TRIANGLE_FRONT_FACE`, the value both targets give a triangle's front.
constexpr u32 k_hit_kind_front_face = 0xfe;
/// What a stage's decision means (CHK-336): 0 accepts, which a plain return does.
constexpr u32 k_decision_ignore = 1;
constexpr u32 k_decision_end_search = 2;
} // namespace

bool lowering::is_ray_stage() const
{
    return _e.entry_stage >= stage::raygen;
}

value_ref lowering::ray_read(built_in system, type_ref type, cc::string_view name)
{
    auto element = -1;
    for (auto i = isize(0); i < _out.stage_elements.size(); ++i)
        if (_out.stage_elements[i].system == system)
            element = i32(i);
    if (element < 0)
        element = i32(add_stage_element({.part = stage_part::input, .system = system, .type = type}, name, name));
    u32 const operands[] = {u32(element)};
    return add(op::stage_load, type, operands);
}

void lowering::read_ray_edge()
{
    // EMIT-139: a traversal function of metal's, which has no stage on these targets
    if (_e.entry_stage == stage::intersection && is_valid(_e.input))
        return unsupported("an intersection fused with its any hit, which only metal writes");
    for (auto i = isize(0); i < _e.stage_inputs.size(); ++i)
    {
        auto const& input = _e.stage_inputs[i];
        if (i < _plan.stage_input_read.size() && !_plan.stage_input_read[i])
            continue;
        auto const is_id = input.input == stage_input::launch_id;
        if (!is_id && input.input != stage_input::launch_size)
            return unsupported("a stage input of no ray-tracing stage");
        auto const declared = value_type(_e.at(input.local).type);
        auto const raw = vector_type(_uint, lanes_of(declared));
        auto const loaded
            = ray_read(is_id ? built_in::launch_id : built_in::launch_size, raw, is_id ? "launch_id" : "launch_size");
        _locals[index_of(input.local)] = add(op::bitcast, declared, {loaded});
    }
    if (is_valid(_e.input) && _e.input != checked_module::void_type)
    {
        auto const type = value_type(_e.input);
        if (!_failure.empty())
            return;
        _locals[0] = add_global({.type = pointer_type(type, storage::ray_input)}, _e.locals[0].name);
        _is_variable[0] = true;
    }
    if (is_valid(_e.attributes))
    {
        auto const position = index_of(_e.attributes);
        _locals[position] = hit_attribute_global(value_type(_e.at(_e.attributes).type));
        _is_variable[position] = true;
    }
}

void lowering::annotate_payloads()
{
    auto candidates = cc::vector<type_id>();
    candidates.push_back(_e.input);
    for (auto const& r : _e.traced_rays)
        candidates.push_back(_m.at(_m.at(_m.at(r.set).type).members)[r.ray].type);
    for (auto const t : candidates)
    {
        if (!is_valid(t) || !emit::impl::is_payload(_plan, t))
            continue;
        auto const type = value_type(t);
        auto is_known = false;
        for (auto const& known : _out.payloads)
            is_known = is_known || known.type == type;
        if (is_known || !_failure.empty())
            continue;
        auto annotation = payload_annotation{.type = type};
        for (auto const& member : _m.at(_m.at(t).members))
            annotation.field_masks.push_back(emit::impl::payload_access_of(_plan, t, member.name).dxil_mask());
        _out.payloads.push_back(cc::move(annotation));
    }
}

value_ref lowering::hit_attribute_global(type_ref type)
{
    if (!ir::is_valid(_hit_attributes))
        _hit_attributes = add_global({.type = pointer_type(type, storage::hit_attribute)}, "sgl_attributes");
    return _hit_attributes;
}

value_ref lowering::output_global(type_ref type, storage space)
{
    auto location = 0;
    for (auto const& known : _ray_outputs)
    {
        if (known.space != space)
            continue;
        if (known.type == type)
            return known.global;
        ++location;
    }
    auto const global = add_global({.type = pointer_type(type, space), .binding = location}, "sgl_out");
    _ray_outputs.push_back({.type = type, .space = space, .global = global});
    return global;
}

value_ref lowering::handed_over(flat_expr_id id, storage space)
{
    auto const place = place_of(id);
    if (!place.has_value())
    {
        unsupported("a payload that is no place");
        return value_ref::none;
    }
    auto const type = value_type(_e.at(id).type);
    if (!_failure.empty())
        return value_ref::none;
    auto const global = output_global(type, space);
    add(op::store, type_ref::none, {global, load(place.value())});
    return global;
}

void lowering::handed_back(flat_expr_id id, value_ref global)
{
    auto const place = place_of(id);
    if (place.has_value())
        store(place.value(), add(op::load, value_type(_e.at(id).type), {global}));
}

bool lowering::trace_operands(cc::span<flat_expr_id const> arguments, storage space, cc::vector<u32>& out)
{
    out.push_back(u32(acceleration_structure(arguments[0])));
    for (auto i = isize(5); i < 10; ++i)
        out.push_back(u32(converted(expr(arguments[i]), _int, _uint)));
    out.push_back(u32(expr(arguments[1])));
    out.push_back(u32(expr(arguments[3])));
    out.push_back(u32(expr(arguments[2])));
    out.push_back(u32(expr(arguments[4])));
    out.push_back(u32(handed_over(arguments[arguments.size() - 1], space)));
    return _failure.empty();
}

void lowering::trace_ray(flat_call const& c)
{
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto operands = cc::vector<u32>();
    if (!trace_operands(arguments, storage::ray_output, operands))
        return;
    add(op::trace_ray, type_ref::none, operands);
    handed_back(arguments[arguments.size() - 1], value_ref(operands[10]));
}

value_ref lowering::hit_object_call(flat_expr const& x, flat_call const& c, builtins::function_record const& record)
{
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto const& name = record.name;
    auto const as_uint = [&](isize i)
    {
        auto const value = expr(arguments[i]);
        return _out.type_of(value) == _uint ? value : converted(value, _int, _uint);
    };
    auto const hit_type = intern({.kind = type_kind::hit_object});
    if (name.starts_with("trace_hit_ray"))
    {
        auto operands = cc::vector<u32>();
        if (!trace_operands(arguments, storage::ray_output, operands))
            return value_ref::none;
        auto const hit = add(op::hit_object_trace, hit_type, operands);
        handed_back(arguments[arguments.size() - 1], value_ref(operands[10]));
        if (_plan.reorders_through_nvapi)
            _traced_structures.push_back({arguments[0], value_ref(operands[0])});
        return hit;
    }
    if (name.starts_with("trace_ray_reordered"))
    {
        auto operands = cc::vector<u32>();
        if (!trace_operands(arguments, storage::ray_output, operands))
            return value_ref::none;
        auto const payload = value_ref(operands[10]);
        if (name.ends_with("_by_hint"))
        {
            operands.push_back(u32(as_uint(10)));
            operands.push_back(u32(as_uint(11)));
        }
        (void)add(op::trace_reorder_execute, hit_type, operands);
        handed_back(arguments[arguments.size() - 1], payload);
        return value_ref::none;
    }
    if (name == "reorder_thread_by_hint")
    {
        add(op::reorder_thread, type_ref::none, {as_uint(0), as_uint(1)});
        return value_ref::none;
    }
    auto const hit = expr(arguments[0]);
    if (!_failure.empty())
        return value_ref::none;
    if (name == "hit_object_reorder")
    {
        add(op::hit_object_reorder, type_ref::none, {hit});
        return value_ref::none;
    }
    if (name == "hit_object_reorder_by_hint")
    {
        add(op::hit_object_reorder, type_ref::none, {hit, as_uint(1), as_uint(2)});
        return value_ref::none;
    }
    if (name == "hit_object_invoke")
    {
        auto const payload = handed_over(arguments[1], storage::ray_output);
        if (!_failure.empty())
            return value_ref::none;
        // EMIT-263: NVAPI invokes through the structure the hit object's trace took, a third operand under its fill
        auto const structure = emit::impl::invoked_structure_of(_plan, arguments[0]);
        auto traced = value_ref::none;
        for (auto const& [named, value] : _traced_structures)
            if (named == structure)
                traced = value;
        if (_plan.reorders_through_nvapi && is_valid(traced))
            add(op::hit_object_invoke, type_ref::none, {hit, payload, traced});
        else
            add(op::hit_object_invoke, type_ref::none, {hit, payload});
        handed_back(arguments[1], payload);
        return value_ref::none;
    }
    struct read
    {
        cc::string_view name;
        hit_object_value what;
    };
    constexpr read k_reads[] = {
        {"hit_object_is_hit", hit_object_value::is_hit},
        {"hit_object_is_miss", hit_object_value::is_miss},
        {"hit_object_t", hit_object_value::t},
        {"hit_object_instance_id", hit_object_value::instance_id},
        {"hit_object_instance_index", hit_object_value::instance_index},
        {"hit_object_geometry_index", hit_object_value::geometry_index},
        {"hit_object_primitive_index", hit_object_value::primitive_index},
    };
    for (auto const& r : k_reads)
        if (r.name == name)
        {
            auto const result = value_type(x.type);
            auto const is_int = result == _int;
            auto const value = add(op::hit_object_read, is_int ? _uint : result, {hit}, u8(r.what));
            return is_int ? add(op::bitcast, _int, {value}) : value;
        }
    unsupported(cc::format("the builtin {}", name));
    return value_ref::none;
}

void lowering::call_callable(flat_call const& c)
{
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto const index = converted(expr(arguments[0]), _int, _uint);
    auto const data = handed_over(arguments[1], storage::callable_output);
    if (!_failure.empty())
        return;
    add(op::execute_callable, type_ref::none, {index, data});
    handed_back(arguments[1], data);
}

value_ref lowering::ray_stage_call(builtins::function_record const& record, cc::span<value_ref const> args, type_ref result)
{
    auto const three = vector_type(_float, 3);
    auto const& name = record.name;
    auto const as_int = [&](built_in system, cc::string_view label)
    { return add(op::bitcast, _int, {ray_read(system, _uint, label)}); };
    auto const row_of = [&](built_in system, cc::string_view label) -> value_ref
    {
        if (!is_constant(args[0]))
        {
            unsupported("a transform's row that is no constant");
            return value_ref::none;
        }
        auto const matrix = intern({.kind = type_kind::matrix, .count = 4, .element = three});
        auto const whole = ray_read(system, matrix, label);
        auto const at = _out.constants[index_of(args[0])].low;
        u32 lanes[4];
        for (auto column = u32(0); column < 4; ++column)
            lanes[column] = u32(extracted(extracted(whole, three, column), _float, at));
        return add(op::construct, vector_type(_float, 4), lanes);
    };
    if (name == "launch_index" || name == "launch_dimensions")
    {
        auto const is_id = name == "launch_index";
        auto const loaded = ray_read(is_id ? built_in::launch_id : built_in::launch_size, vector_type(_uint, 3),
                                     is_id ? "launch_id" : "launch_size");
        return add(op::bitcast, result, {loaded});
    }
    if (name == "world_ray_origin")
        return ray_read(built_in::world_ray_origin, three, name);
    if (name == "world_ray_direction")
        return ray_read(built_in::world_ray_direction, three, name);
    if (name == "hit_object_ray_origin")
        return ray_read(built_in::object_ray_origin, three, name);
    if (name == "hit_object_ray_direction")
        return ray_read(built_in::object_ray_direction, three, name);
    if (name == "ray_t_min")
        return ray_read(built_in::ray_t_min, _float, name);
    if (name == "ray_t_current")
        return ray_read(built_in::ray_t_current, _float, name);
    if (name == "hit_instance_id")
        return as_int(built_in::ray_instance_id, name);
    if (name == "hit_instance_index")
        return as_int(built_in::ray_instance_index, name);
    if (name == "hit_geometry_index")
        return as_int(built_in::ray_geometry_index, name);
    if (name == "hit_primitive_index")
        return as_int(built_in::ray_primitive_index, name);
    if (name == "hit_cluster_id")
        return as_int(built_in::ray_cluster_id, name);
    if (name == "hit_object_to_world_row")
        return row_of(built_in::object_to_world, "object_to_world");
    if (name == "hit_world_to_object_row")
        return row_of(built_in::world_to_object, "world_to_object");
    if (name == "hit_is_front_face")
        return add(op::ieq, _bool, {ray_read(built_in::hit_kind, _uint, name), uint_constant(k_hit_kind_front_face)});
    if (name == "hit_barycentrics")
        return add(op::load, result, {hit_attribute_global(result)});
    unsupported(cc::format("the builtin {}", name));
    return value_ref::none;
}

void lowering::decide_hit(value_ref decision)
{
    if (is_constant(decision))
    {
        auto const said = _out.constants[index_of(decision)].low;
        if (said == k_decision_ignore)
            add(op::ignore_hit, type_ref::none, cc::span<u32 const>());
        else if (said == k_decision_end_search)
            add(op::accept_and_end_search, type_ref::none, cc::span<u32 const>());
        return;
    }
    auto const when = [&](u32 said, op ending)
    {
        auto const then_block = new_block();
        auto const merge = new_block();
        set_merge(merge_kind::selection, merge);
        branch_if(add(op::ieq, _bool, {decision, int_constant(i32(said))}), then_block, merge);
        open_block(then_block);
        add(ending, type_ref::none, cc::span<u32 const>());
        open_block(merge);
    };
    when(k_decision_ignore, op::ignore_hit);
    when(k_decision_end_search, op::accept_and_end_search);
}

void lowering::report_intersection(value_ref reported, type_id type)
{
    auto const position_of = [&](cc::string_view member) -> u32
    {
        auto at = u32(0);
        for (auto const& m : _m.at(_m.at(type).members))
        {
            if (m.type == checked_module::void_type)
                continue;
            if (m.name == member)
                return at;
            ++at;
        }
        return 0;
    };
    auto const is_hit = extracted(reported, _bool, position_of("is_hit"));
    auto const then_block = new_block();
    auto const merge = new_block();
    set_merge(merge_kind::selection, merge);
    branch_if(is_hit, then_block, merge);
    open_block(then_block);
    auto const attributes_type = value_type(_m.at(type).element);
    auto const attributes = hit_attribute_global(attributes_type);
    add(op::store, type_ref::none, {attributes, extracted(reported, attributes_type, position_of("attributes"))});
    auto const t = extracted(reported, _float, position_of("t"));
    add(op::report_hit, _bool, {t, uint_constant(0), attributes});
    branch(merge);
    open_block(merge);
}
