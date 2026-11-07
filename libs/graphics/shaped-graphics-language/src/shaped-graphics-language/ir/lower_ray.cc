#include <clean-core/string/format.hh>
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

void lowering::trace_ray(flat_call const& c)
{
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto const world = acceleration_structure(arguments[0]);
    auto const as_uint = [&](isize i) { return u32(converted(expr(arguments[i]), _int, _uint)); };
    u32 operands[11];
    operands[0] = u32(world);
    for (auto i = isize(0); i < 5; ++i)
        operands[i + 1] = as_uint(i + 5);
    operands[6] = u32(expr(arguments[1]));
    operands[7] = u32(expr(arguments[3]));
    operands[8] = u32(expr(arguments[2]));
    operands[9] = u32(expr(arguments[4]));
    auto const payload = handed_over(arguments[10], storage::ray_output);
    if (!_failure.empty())
        return;
    operands[10] = u32(payload);
    add(op::trace_ray, type_ref::none, operands);
    handed_back(arguments[10], payload);
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
