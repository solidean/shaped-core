#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ir/impl/lowering.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::impl;

using check::stage;
using check::stage_input;

namespace
{
[[nodiscard]] built_in built_in_of(stage_input input)
{
    switch (input)
    {
    case stage_input::thread_id:
        return built_in::thread_id;
    case stage_input::local_thread_id:
        return built_in::local_thread_id;
    case stage_input::local_thread_index:
        return built_in::local_thread_index;
    case stage_input::workgroup_id:
        return built_in::workgroup_id;
    case stage_input::subgroup_size:
        return built_in::subgroup_size;
    case stage_input::subgroup_invocation_id:
        return built_in::subgroup_invocation_id;
    case stage_input::vertex_index:
        return built_in::vertex_index;
    case stage_input::instance_index:
        return built_in::instance_index;
    case stage_input::first_instance:
        return built_in::first_instance;
    case stage_input::is_front_facing:
        return built_in::is_front_facing;
    case stage_input::sample_index:
        return built_in::sample_index;
    case stage_input::sample_mask:
        return built_in::sample_mask;
    case stage_input::primitive_id:
        return built_in::primitive_id;
    case stage_input::view_index:
        return built_in::view_index;
    case stage_input::domain_location:
        return built_in::domain_location;
    case stage_input::launch_id:
    case stage_input::launch_size:
    case stage_input::none:
        break;
    }
    return built_in::none;
}

/// The name and the number HLSL reads a semantic as: `SGL10` is `SGL` at 10.
struct split_semantic
{
    cc::string_view name;
    u32 index = 0;
};
[[nodiscard]] split_semantic split(cc::string_view semantic)
{
    auto end = semantic.size();
    while (end > 0 && semantic[end - 1] >= '0' && semantic[end - 1] <= '9')
        --end;
    auto index = u32(0);
    for (auto i = end; i < semantic.size(); ++i)
        index = index * 10 + u32(semantic[i] - '0');
    return {.name = semantic.subview({.offset = 0, .size = end}), .index = index};
}

[[nodiscard]] interpolation mode_of(check::interpolation const& i)
{
    using kind = check::interpolation::kind_t;
    using sampling = check::interpolation::sampling_t;
    if (i.kind == kind::flat)
        return interpolation::flat;
    auto const is_linear = i.kind == kind::linear;
    if (i.sampling == sampling::centroid)
        return is_linear ? interpolation::noperspective_centroid : interpolation::centroid;
    if (i.sampling == sampling::sample)
        return is_linear ? interpolation::noperspective_sample : interpolation::sample;
    return is_linear ? interpolation::noperspective : interpolation::perspective;
}
} // namespace

u32 lowering::add_stage_element(stage_element e, cc::string_view semantic, cc::string_view name)
{
    auto const parts = split(semantic);
    e.semantic_first = add_name(parts.name);
    e.semantic_size = u32(parts.name.size());
    e.semantic_index = parts.index;
    e.name_first = add_name(name);
    e.name_size = u32(name.size());
    _out.stage_elements.push_back(e);
    return u32(_out.stage_elements.size() - 1);
}

void lowering::elements_of_struct(check::type_id t, stage_part part, u32 array_size)
{
    emit::impl::planned_struct const* found = nullptr;
    for (auto const& s : _plan.structs)
        if (s.type == t)
            found = &s;
    if (found == nullptr)
        return;
    auto const& s = *found;
    for (auto const& member : emit::impl::declared_members(s))
    {
        auto e = stage_element{.part = part, .location = member.location, .array_size = array_size};
        if (member.is_position)
            e.system = built_in::position;
        else if (member.is_layer)
            e.system = built_in::layer;
        else if (member.factor == check::tessellation_factor::edge)
            e.system = built_in::tess_edge;
        else if (member.factor == check::tessellation_factor::inside)
            e.system = built_in::tess_inside;
        else if (s.role == emit::impl::struct_role::render_targets)
            switch (member.output)
            {
            case check::pixel_output::depth:
                e.system = built_in::depth;
                break;
            case check::pixel_output::depth_greater_equal:
                e.system = built_in::depth_greater_equal;
                break;
            case check::pixel_output::depth_less_equal:
                e.system = built_in::depth_less_equal;
                break;
            case check::pixel_output::sample_mask:
                e.system = built_in::sample_mask;
                break;
            case check::pixel_output::color:
                break;
            }
        auto const semantic = emit::impl::semantic_of(_plan, s, member);
        // a factor is an array of the tessellator's floats, one element per factor
        auto const& declared = _m.at(member.type);
        if (declared.kind == check::type_kind::array)
        {
            e.type = value_type(declared.element);
            for (auto i = u32(0); i < declared.count; ++i)
            {
                add_stage_element(e, semantic, member.source_name);
                _out.stage_elements.back().semantic_index = i;
            }
            continue;
        }
        e.type = value_type(member.type);
        auto const& held = _out.at(e.type);
        auto const& scalar = held.kind == ir::type_kind::vector ? _out.at(held.element) : held;
        // CHK-488: a primitive has one value of it, so nothing interpolates
        if (s.role == emit::impl::struct_role::primitive_link)
            e.interpolate = interpolation::flat;
        else if (e.system == built_in::none && s.role == emit::impl::struct_role::stage_link)
            e.interpolate = scalar.kind == ir::type_kind::float_ ? mode_of(member.interpolate) : interpolation::flat;
        if (member.is_position)
            e.interpolate = interpolation::noperspective;
        else if (member.is_layer)
            e.interpolate = interpolation::flat;
        add_stage_element(e, semantic, member.source_name);
    }
}

u32 lowering::element_of_input(isize index)
{
    auto const& input = _e.stage_inputs[index];
    auto const& spelled = emit::impl::spelling_of(input.input);
    auto const declared = value_type(_e.at(input.local).type);
    auto const lanes = _out.at(declared).kind == ir::type_kind::vector ? _out.at(declared).count : 1;
    auto type = input.input == stage_input::is_front_facing ? _bool : lanes == 1 ? _uint : vector_type(_uint, lanes);
    if (input.input == stage_input::domain_location)
        type = declared;
    auto const is_system_mode = _e.entry_stage == stage::pixel
                             && (input.input == stage_input::primitive_id || input.input == stage_input::is_front_facing
                                 || input.input == stage_input::sample_index);
    return add_stage_element({.part = stage_part::input,
                              .system = built_in_of(input.input),
                              .type = type,
                              .interpolate = is_system_mode ? interpolation::flat : interpolation::none},
                             spelled.hlsl_semantic, _plan.stage_input_names[index]);
}

value_ref lowering::read_element(u32 element)
{
    u32 const operands[] = {element};
    return add(op::stage_load, _out.stage_elements[element].type, operands);
}

value_ref lowering::read_struct(check::type_id t, u32& element)
{
    emit::impl::planned_struct const* found = nullptr;
    for (auto const& s : _plan.structs)
        if (s.type == t)
            found = &s;
    auto const type = value_type(t);
    if (found == nullptr || found->members.empty())
        return null_of(type);
    auto parts = cc::vector<u32>();
    for (auto const& member : found->members)
    {
        if (member.pieces == 0)
        {
            parts.push_back(u32(read_element(element++)));
            continue;
        }
        // EMIT-174: an array or a matrix crosses as its pieces
        auto pieces = cc::vector<u32>();
        for (auto i = 0; i < member.pieces; ++i)
            pieces.push_back(u32(read_element(element++)));
        parts.push_back(u32(add(op::construct, value_type(member.type), pieces)));
    }
    return add(op::construct, type, parts);
}

void lowering::hold_parameter(check::local_id id, value_ref value)
{
    auto const& local = _e.at(id);
    auto const type = value_type(local.type);
    auto const position = index_of(id);
    // an array is reached by an index, which only a variable can take
    if (!local.is_mut && !holds_array(type))
    {
        _locals[position] = value;
        return;
    }
    auto const variable = add_local(type, local.name);
    _is_variable[position] = true;
    _locals[position] = variable;
    add(op::store, type_ref::none, {variable, value});
}

void lowering::read_stage_edge()
{
    for (auto const& e : _out.stage_elements)
    {
        auto const is_read_here = e.system == built_in::none || e.system == built_in::position
                               || e.system == built_in::vertex_index || e.system == built_in::instance_index
                               || e.system == built_in::first_instance;
        if (!is_read_here)
            return unsupported(cc::format("the stage element {}", _out.name_at(e.name_first, e.name_size)));
        if (e.part == stage_part::primitive)
            return unsupported("what a mesh stage wrote per primitive");
        auto const& held = _out.at(e.type);
        auto const& scalar = held.kind == ir::type_kind::vector ? _out.at(held.element) : held;
        if (scalar.width != 32 || scalar.kind == ir::type_kind::bool_)
            return unsupported("a link that is no 32-bit number");
    }
    // EMIT-196: a 64-bit value crossing as its words stands in 32-bit elements, and the check above never sees it
    for (auto const& s : _plan.structs)
        if (s.type == _e.input || s.type == _e.result)
            for (auto const& member : s.members)
                if (emit::impl::is_word_split(_plan, member.type))
                    return unsupported("a 64-bit link, which crosses as its words");

    auto element = u32(0);
    if (check::is_valid(_e.input))
        hold_parameter(check::local_id(0), read_struct(_e.input, element));
    for (auto i = isize(0); i < _e.stage_inputs.size(); ++i, ++element)
    {
        if (i < _plan.stage_input_read.size() && !_plan.stage_input_read[i])
            continue;
        auto const& input = _e.stage_inputs[i];
        auto const raw = _out.stage_elements[element].type;
        auto const loaded = read_element(element);
        auto const declared = value_type(_e.at(input.local).type);
        hold_parameter(input.local, raw == declared ? loaded : add(op::bitcast, declared, {loaded}));
    }
}

void lowering::store_stage_result(value_ref result)
{
    emit::impl::planned_struct const* found = nullptr;
    for (auto const& s : _plan.structs)
        if (s.type == _e.result)
            found = &s;
    if (found == nullptr)
        return unsupported("a stage result that is no struct");
    auto element = u32(0);
    while (element < _out.stage_elements.size() && _out.stage_elements[element].part != stage_part::output)
        ++element;
    auto const store_element = [&](value_ref value)
    {
        u32 const operands[] = {element++, u32(value)};
        add(op::stage_store, type_ref::none, operands);
    };
    for (auto m = isize(0); m < found->members.size(); ++m)
    {
        auto const& member = found->members[m];
        auto const whole = extracted(result, value_type(member.type), u32(m));
        if (member.pieces == 0)
        {
            store_element(whole);
            continue;
        }
        for (auto i = 0; i < member.pieces; ++i)
            store_element(extracted(whole, value_type(member.piece), u32(i)));
    }
}

void lowering::declare_stage_elements()
{
    auto const input_of_local = [&](isize local)
    {
        for (auto k = isize(0); k < _e.stage_inputs.size(); ++k)
            if (index_of(_e.stage_inputs[k].local) == local)
                return k;
        return isize(-1);
    };

    if (_e.entry_stage == stage::vertex || _e.entry_stage == stage::pixel)
    {
        if (check::is_valid(_e.input))
            elements_of_struct(_e.input, stage_part::input, 0);
        // CHK-489: a pixel stage's second struct, what a mesh stage wrote per triangle
        if (_e.entry_stage == stage::pixel && check::is_valid(_e.input))
            for (auto i = isize(1); i < _e.locals.size(); ++i)
            {
                auto const& local = _e.locals[i];
                if (local.kind == check::local_kind::parameter && input_of_local(i) < 0
                    && _m.at(local.type).kind == check::type_kind::structure && local.type != _e.input)
                    elements_of_struct(local.type, stage_part::primitive, 0);
            }
        for (auto i = isize(0); i < _e.stage_inputs.size(); ++i)
            element_of_input(i);
        if (check::is_valid(_e.result) && _e.result != check::checked_module::void_type)
            elements_of_struct(_e.result, stage_part::output, 0);
        // EMIT-130: DXIL takes a conservative depth only from a stage whose position is interpolated at the centroid
        auto is_conservative = false;
        for (auto const& e : _out.stage_elements)
            is_conservative = is_conservative || e.system == built_in::depth_greater_equal
                           || e.system == built_in::depth_less_equal;
        for (auto& e : _out.stage_elements)
            if (is_conservative && e.system == built_in::position)
                e.interpolate = interpolation::noperspective_centroid;
        return;
    }
    if (_e.entry_stage != stage::geometry && _e.entry_stage != stage::tessellation_control
        && _e.entry_stage != stage::tessellation_evaluation && _e.entry_stage != stage::mesh)
        return;

    // EMIT-123: these stages' parameters, in the order written
    for (auto i = isize(0); i < _e.locals.size(); ++i)
    {
        auto const& local = _e.locals[i];
        if (local.kind != check::local_kind::parameter)
            continue;
        auto const& t = _m.at(local.type);
        if (auto const input = input_of_local(i); input >= 0)
            element_of_input(input);
        else if (t.kind == check::type_kind::array)
        {
            elements_of_struct(t.element, stage_part::input, t.count);
            // a control stage's control points pass through unchanged
            if (_e.entry_stage == stage::tessellation_control)
                elements_of_struct(t.element, stage_part::output, t.count);
        }
        else if (t.kind == check::type_kind::stream && t.count == 0)
        {
            auto const& mesh = _m.functions[_m.at(_e.function).info];
            elements_of_struct(t.element, stage_part::output, mesh.max_vertices);
            if (check::is_valid(t.primitive))
                elements_of_struct(t.primitive, stage_part::primitive, mesh.max_primitives);
        }
        else if (t.kind == check::type_kind::stream)
            elements_of_struct(t.element, stage_part::output, 0);
        else if (_e.entry_stage != stage::mesh)
            elements_of_struct(local.type, stage_part::patch, 0);
    }
    if (_e.entry_stage == stage::tessellation_control)
        elements_of_struct(_e.result, stage_part::patch, 0);
    else if (_e.entry_stage == stage::tessellation_evaluation)
        elements_of_struct(_e.result, stage_part::output, 0);
}
