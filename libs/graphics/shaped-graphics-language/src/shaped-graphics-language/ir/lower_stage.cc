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
        auto const held = _out.at(e.type);
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
    {
        type = declared;
        // SPIR-V's `TessCoord` is three lanes whatever the domain, and that writer looks the type up
        (void)vector_type(_float, 3);
    }
    auto const is_system_mode = _e.entry_stage == stage::pixel
                             && (input.input == stage_input::primitive_id || input.input == stage_input::is_front_facing
                                 || input.input == stage_input::sample_index);
    return add_stage_element({.part = stage_part::input,
                              .system = built_in_of(input.input),
                              .type = type,
                              .interpolate = is_system_mode ? interpolation::flat : interpolation::none},
                             spelled.hlsl_semantic, _plan.stage_input_names[index]);
}

value_ref lowering::read_element(u32 element, value_ref index)
{
    u32 const operands[] = {element, u32(index)};
    auto const count = _out.stage_elements[element].array_size > 0 ? 2 : 1;
    return add(op::stage_load, _out.stage_elements[element].type, cc::span<u32 const>(operands, count));
}

value_ref lowering::read_struct(check::type_id t, u32& element, value_ref index)
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
        // a factor crosses as one element per factor, where every other array crosses as pieces
        if (member.factor != check::tessellation_factor::none && _m.at(member.type).kind == check::type_kind::array)
        {
            auto factors = cc::vector<u32>();
            for (auto i = u32(0); i < u32(_m.at(member.type).count); ++i)
                factors.push_back(u32(read_element(element++, index)));
            parts.push_back(u32(add(op::construct, value_type(member.type), factors)));
            continue;
        }
        if (member.pieces == 0)
        {
            parts.push_back(u32(read_element(element++, index)));
            continue;
        }
        if (emit::impl::is_word_split(_plan, member.type))
        {
            parts.push_back(u32(joined_words(member.type, element, index)));
            continue;
        }
        if (_m.at(member.type).kind == check::type_kind::array
            && emit::impl::is_word_split(_plan, _m.at(member.type).element))
        {
            auto joined = cc::vector<u32>();
            for (auto i = u32(0); i < _m.at(member.type).count; ++i)
                joined.push_back(u32(joined_words(_m.at(member.type).element, element, index)));
            parts.push_back(u32(add(op::construct, value_type(member.type), joined)));
            continue;
        }
        // EMIT-174: an array or a matrix crosses as its pieces
        auto pieces = cc::vector<u32>();
        for (auto i = 0; i < member.pieces; ++i)
            pieces.push_back(u32(read_element(element++, index)));
        parts.push_back(u32(add(op::construct, value_type(member.type), pieces)));
    }
    return add(op::construct, type, parts);
}

value_ref lowering::joined_words(check::type_id t, u32& element, value_ref index)
{
    auto const type = value_type(t);
    auto const scalar = scalar_of(type);
    auto const lanes = lanes_of(type);
    auto pieces = cc::vector<value_ref>();
    for (auto i = 0; i < emit::impl::word_pieces(_plan, t); ++i)
        pieces.push_back(read_element(element++, index));
    auto const pair = vector_type(_uint, 2);
    auto words = cc::vector<u32>();
    for (auto lane = u32(0); lane < lanes; ++lane)
    {
        auto const piece = pieces[lane / 2];
        auto held = piece;
        if (lanes_of(_out.type_of(piece)) != 2)
        {
            auto const first = lane % 2 * 2;
            u32 const operands[] = {u32(piece), u32(piece), first, first + 1};
            held = add(op::shuffle, pair, operands);
        }
        words.push_back(u32(add(op::bitcast, scalar, {held})));
    }
    return lanes == 1 ? value_ref(words[0]) : add(op::construct, type, words);
}

void lowering::store_words(check::type_id t, value_ref value, u32& element, value_ref index)
{
    auto const type = value_type(t);
    auto const scalar = scalar_of(type);
    auto const lanes = lanes_of(type);
    auto const pair = vector_type(_uint, 2);
    auto const store_element = [&](value_ref word)
    {
        u32 const operands[] = {element++, u32(word), u32(index)};
        add(op::stage_store, type_ref::none, cc::span<u32 const>(operands, is_valid(index) ? 3 : 2));
    };
    auto const pair_at = [&](u32 lane)
    {
        // a three-vector's last piece is two words and then two zeros
        if (lane >= lanes)
            return null_of(pair);
        return add(op::bitcast, pair, {lanes == 1 ? value : extracted(value, scalar, lane)});
    };
    if (lanes == 1)
        return store_element(pair_at(0));
    for (auto lane = u32(0); lane < lanes; lane += 2)
        store_element(add(op::construct, vector_type(_uint, 4), {pair_at(lane), pair_at(lane + 1)}));
}

bool lowering::is_primitive_parameter(isize position) const
{
    if (_e.entry_stage != stage::pixel || position < 1 || !check::is_valid(_e.input))
        return false;
    auto const& local = _e.locals[position];
    if (local.kind != check::local_kind::parameter || _m.at(local.type).kind != check::type_kind::structure
        || local.type == _e.input)
        return false;
    for (auto const& input : _e.stage_inputs)
        if (index_of(input.local) == position)
            return false;
    return true;
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

void lowering::check_links()
{
    for (auto const& e : _out.stage_elements)
    {
        auto const held = _out.at(e.type);
        auto const& scalar = held.kind == ir::type_kind::vector ? _out.at(held.element) : held;
        if (e.system == built_in::none && (scalar.width < 32 || scalar.kind == ir::type_kind::bool_))
            return unsupported("a link that is no 32-bit or 64-bit number");
    }
}

void lowering::read_stage_edge()
{
    check_links();
    auto element = u32(0);
    if (check::is_valid(_e.input))
        hold_parameter(check::local_id(0), read_struct(_e.input, element));
    for (auto i = isize(1); i < _e.locals.size(); ++i)
        if (is_primitive_parameter(i))
            hold_parameter(check::local_id(i), read_struct(_e.locals[i].type, element));
    for (auto i = isize(0); i < _e.stage_inputs.size(); ++i, ++element)
        read_stage_input(i, element);
}

void lowering::read_stage_input(isize input, u32 element)
{
    if (input < _plan.stage_input_read.size() && !_plan.stage_input_read[input])
        return;
    auto const local = _e.stage_inputs[input].local;
    auto const raw = _out.stage_elements[element].type;
    auto const loaded = read_element(element);
    auto const declared = value_type(_e.at(local).type);
    hold_parameter(local, raw == declared ? loaded : add(op::bitcast, declared, {loaded}));
}

void lowering::describe_primitive_stage()
{
    auto& shape = _out.primitive;
    auto const& info = _m.functions[_m.at(_e.function).info];
    for (auto const& local : _e.locals)
    {
        if (local.kind != check::local_kind::parameter)
            continue;
        auto const& t = _m.at(local.type);
        if (t.kind == check::type_kind::array)
            shape.input_vertices = u32(t.count);
        else if (t.kind == check::type_kind::stream)
            shape.output_vertices = u32(t.count);
    }
    shape.max_vertices = u32(info.max_vertices);
    if (_e.entry_stage == stage::geometry)
        return;
    if (_e.entry_stage == stage::mesh)
    {
        shape.max_primitives = u32(info.max_primitives);
        shape.writes_lines = info.is_line_mesh;
        shape.primitive_indices = vector_type(_uint, info.is_line_mesh ? 2 : 3);
        return;
    }
    auto edges = 0;
    for (auto const& e : _out.stage_elements)
        edges += e.system == built_in::tess_edge ? 1 : 0;
    if (edges < 2 || edges > 4)
        return unsupported("a tessellation stage whose factors name no domain");
    shape.domain = edges == 2 ? tessellation_domain::isoline
                 : edges == 3 ? tessellation_domain::triangle
                              : tessellation_domain::quad;
    shape.partitioning = info.partitioning;
    shape.is_output_clockwise = !info.is_clockwise;
    shape.max_factor = u32(info.max_factor);
    if (_e.entry_stage == stage::tessellation_control)
    {
        shape.body_name_first = add_name(_plan.patch_function);
        shape.body_name_size = u32(_plan.patch_function.size());
    }
}

void lowering::read_primitive_edge()
{
    check_links();
    for (auto i = isize(0); i < _e.locals.size() && _failure.empty(); ++i)
    {
        auto const first = _parameter_elements[i];
        auto const kind = _m.at(_e.locals[i].type).kind;
        if (first == k_no_element || kind == check::type_kind::array || kind == check::type_kind::stream)
            continue;
        auto input = isize(-1);
        for (auto k = isize(0); k < _e.stage_inputs.size(); ++k)
            if (index_of(_e.stage_inputs[k].local) == i)
                input = k;
        if (input >= 0)
        {
            read_stage_input(input, first);
            continue;
        }
        // EMIT-242: what a task stage handed on is read through the place it stands in, so an index copies nothing
        if (_e.entry_stage == stage::mesh)
        {
            _is_variable[i] = true;
            _locals[i] = payload_global(_e.locals[i].type);
            continue;
        }
        auto element = first;
        hold_parameter(check::local_id(i), read_struct(_e.locals[i].type, element));
    }
}

u32 lowering::stage_array_of(flat_expr_id id) const
{
    auto const* const l = _e.at(id).node.try_as<check::flat_local_ref>();
    if (l == nullptr || _m.at(_e.at(l->local).type).kind != check::type_kind::array)
        return k_no_element;
    return _parameter_elements[index_of(l->local)];
}

value_ref lowering::read_stage_array(check::type_id t, u32 first)
{
    auto const& array = _m.at(t);
    auto parts = cc::vector<u32>();
    for (auto i = u32(0); i < u32(array.count); ++i)
    {
        auto element = first;
        parts.push_back(u32(read_struct(array.element, element, uint_constant(i))));
    }
    return add(op::construct, value_type(t), parts);
}

void lowering::stream_call(check::flat_call const& c, bool is_emit)
{
    if (!is_emit)
    {
        add(op::end_strip, type_ref::none, cc::span<u32 const>());
        return;
    }
    auto const arguments = _e.at(c.arguments);
    store_struct(_m.at(_e.at(arguments[0]).type).element, expr(arguments[1]), stage_part::output);
    add(op::emit_vertex, type_ref::none, cc::span<u32 const>());
}

void lowering::store_stage_result(value_ref result)
{
    store_struct(_e.result, result,
                 _e.entry_stage == stage::tessellation_control ? stage_part::patch : stage_part::output);
}

void lowering::mesh_call(check::flat_call const& c, cc::string_view method)
{
    auto const arguments = _e.at(c.arguments);
    auto const& output = _m.at(_e.at(arguments[0]).type);
    auto const first = expr(arguments[1]);
    auto const second = expr(arguments[2]);
    if (method == "set_counts")
        add(op::set_mesh_counts, type_ref::none, {first, second});
    else if (method == "vertex")
        store_struct(output.element, second, stage_part::output, first);
    else if (method == "primitive")
        store_struct(output.primitive, second, stage_part::primitive, first);
    else
        add(op::write_primitive, type_ref::none, {first, second});
}

value_ref lowering::payload_global(check::type_id t)
{
    if (!is_valid(_payload))
        _payload = add_global({.type = pointer_type(value_type(t), storage::task_payload)}, "sgl_payload");
    return _payload;
}

void lowering::dispatch_mesh(check::flat_call const& c)
{
    auto const arguments = _e.at(c.arguments);
    auto const x = expr(arguments[0]);
    auto const y = expr(arguments[1]);
    auto const z = expr(arguments[2]);
    auto const payload = payload_global(_e.at(arguments[3]).type);
    add(op::store, type_ref::none, {payload, expr(arguments[3])});
    u32 const operands[] = {u32(x), u32(y), u32(z), u32(payload)};
    add(op::dispatch_mesh, type_ref::none, operands);
}

void lowering::store_struct(check::type_id t, value_ref result, stage_part part, value_ref index)
{
    emit::impl::planned_struct const* found = nullptr;
    for (auto const& s : _plan.structs)
        if (s.type == t)
            found = &s;
    if (found == nullptr)
        return unsupported("a stage result that is no struct");
    auto element = u32(0);
    while (element < _out.stage_elements.size() && _out.stage_elements[element].part != part)
        ++element;
    auto const store_element = [&](value_ref value)
    {
        u32 const operands[] = {element++, u32(value), u32(index)};
        add(op::stage_store, type_ref::none, cc::span<u32 const>(operands, is_valid(index) ? 3 : 2));
    };
    for (auto m = isize(0); m < found->members.size(); ++m)
    {
        auto const& member = found->members[m];
        auto const whole = extracted(result, value_type(member.type), u32(m));
        if (member.factor != check::tessellation_factor::none && _m.at(member.type).kind == check::type_kind::array)
        {
            for (auto i = u32(0); i < u32(_m.at(member.type).count); ++i)
                store_element(extracted(whole, value_type(_m.at(member.type).element), i));
            continue;
        }
        if (member.pieces == 0)
        {
            store_element(whole);
            continue;
        }
        if (emit::impl::is_word_split(_plan, member.type))
        {
            store_words(member.type, whole, element, index);
            continue;
        }
        if (_m.at(member.type).kind == check::type_kind::array
            && emit::impl::is_word_split(_plan, _m.at(member.type).element))
        {
            auto const held = _m.at(member.type).element;
            for (auto i = u32(0); i < _m.at(member.type).count; ++i)
                store_words(held, extracted(whole, value_type(held), i), element, index);
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
        for (auto i = isize(1); i < _e.locals.size(); ++i)
            if (is_primitive_parameter(i))
                elements_of_struct(_e.locals[i].type, stage_part::primitive, 0);
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
        _parameter_elements[i] = u32(_out.stage_elements.size());
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
