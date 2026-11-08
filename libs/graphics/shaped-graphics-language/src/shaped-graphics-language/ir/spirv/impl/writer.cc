#include "writer.hh"

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::spirv::impl;

cc::vector<u32> writer::run()
{
    number_everything();
    write_types();
    write_constants();
    write_stage_elements();
    write_globals();
    find_scalings();
    write_function();
    if (_p.stage == check::stage::tessellation_control)
        write_control_entry();
    write_entry();
    declare_files();
    return assembled();
}

void writer::number_everything()
{
    auto next = u32(1);
    auto const take = [&](isize count)
    {
        auto const first = next;
        next += u32(count);
        return first;
    };
    _types = take(_p.types.size());
    _constants = take(_p.constants.size());
    _elements = take(_p.stage_elements.size());
    _globals = take(_p.globals.size());
    _locals = take(_p.locals.size());
    _instructions = take(_p.instructions.size());
    _blocks = take(_p.blocks.size());
    _glsl = take(1);
    _function = take(1);
    _body = _p.stage == check::stage::tessellation_control ? take(1) : _function;
    _function_type = take(1);
    _bound = next;
}

void writer::string_into(cc::vector<u32>& out, cc::string_view text)
{
    auto word = u32(0);
    auto shift = 0;
    for (auto const c : text)
    {
        word |= u32(u8(c)) << shift;
        shift += 8;
        if (shift == 32)
        {
            out.push_back(word);
            word = 0;
            shift = 0;
        }
    }
    out.push_back(word);
}

void writer::name(u32 id, cc::string_view text)
{
    if (text.empty())
        return;
    head(_names, spv::Name, 2 + string_words(text));
    _names.push_back(id);
    string_into(_names, text);
}

void writer::require(u32 capability)
{
    for (auto const c : _capabilities)
        if (c == capability)
            return;
    _capabilities.push_back(capability);
}

void writer::require_extension(cc::string_view text)
{
    for (auto const e : _extensions)
        if (e == text)
            return;
    _extensions.push_back(text);
}

u32 writer::literal_id(u32 value)
{
    for (auto const& known : _literals)
        if (known.value == value)
            return known.id;
    auto const id = _bound++;
    _literals.push_back({.value = value, .id = id});
    emit(_declarations, spv::Constant, {id_of(_uint), id, value});
    return id;
}

u32 writer::constant_composite(value_ref v)
{
    if (is_constant(v))
        return id_of(v);
    auto const& made = _p.instructions[u32(v)];
    auto const parts = _p.operands_of(made);
    auto const id = _bound++;
    head(_declarations, spv::ConstantComposite, 3 + parts.size());
    _declarations.push_back(id_of(made.type));
    _declarations.push_back(id);
    for (auto const part : parts)
        _declarations.push_back(id_of_raw(part));
    return id;
}

void writer::terminator(ir::block const& b, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    if (b.merges == merge_kind::selection)
        emit(_code, spv::SelectionMerge, {id_of(b.merge), 0u});
    else if (b.merges == merge_kind::loop)
        emit(_code, spv::LoopMerge, {id_of(b.merge), id_of(b.continue_to), 0u});

    switch (i.code)
    {
    case op::branch:
        return emit(_code, spv::Branch, {id_of(block_ref(operands[0]))});
    case op::branch_conditional:
        return emit(_code, spv::BranchConditional,
                    {id_of_raw(operands[0]), id_of(block_ref(operands[1])), id_of(block_ref(operands[2]))});
    case op::switch_:
    {
        head(_code, spv::Switch, 1 + operands.size());
        _code.push_back(id_of_raw(operands[0]));
        _code.push_back(id_of(block_ref(operands[1])));
        for (auto k = isize(2); k < operands.size(); k += 2)
        {
            _code.push_back(operands[k]);
            _code.push_back(id_of(block_ref(operands[k + 1])));
        }
        return;
    }
    case op::dispatch_mesh:
    {
        // one payload of the workgroup, so each invocation's store stands before the mesh stage reads it
        constexpr u32 k_scope_workgroup = 2;
        constexpr u32 k_semantics_workgroup_acquire_release = 0x108;
        if (_p.workgroup[0] * _p.workgroup[1] * _p.workgroup[2] > 1)
            emit(_code, spv::ControlBarrier,
                 {literal_id(k_scope_workgroup), literal_id(k_scope_workgroup),
                  literal_id(k_semantics_workgroup_acquire_release)});
        return emit(_code, spv::EmitMeshTasksEXT,
                    {id_of_raw(operands[0]), id_of_raw(operands[1]), id_of_raw(operands[2]), id_of_raw(operands[3])});
    }
    case op::ignore_hit:
        return emit(_code, spv::IgnoreIntersectionKHR, {});
    case op::accept_and_end_search:
        return emit(_code, spv::TerminateRayKHR, {});
    case op::return_:
        return emit(_code, spv::Return, {});
    default:
        return emit(_code, spv::Unreachable, {});
    }
}

void writer::write_function()
{
    emit(_code, spv::Function, {id_of(_void), _body, 0u, _function_type});
    auto const order = blocks_in_order(_p);
    for (auto k = isize(0); k < order.size(); ++k)
    {
        auto const& b = _p.blocks[isize(order[k])];
        // a block a pass dropped, which nothing names
        if (b.count == 0)
            continue;
        emit(_code, spv::Label, {id_of(order[k])});
        forget_line();
        if (k == 0)
            for (auto i = isize(0); i < _p.locals.size(); ++i)
            {
                emit(_code, spv::Variable, {id_of(_p.locals[i].type), _locals + u32(i), spv::FunctionStorage});
                name(_locals + u32(i), _p.name_at(_p.locals[i].name_first, _p.locals[i].name_size));
            }
        // a query and a hit object are variables SPIR-V reaches by their pointer, however deep in the body they begin
        if (k == 0)
            for (auto i = isize(0); i < _p.instructions.size(); ++i)
                if (_p.instructions[i].code == op::ray_query_begin || _p.instructions[i].code == op::hit_object_trace
                    || _p.instructions[i].code == op::trace_reorder_execute)
                    emit(_code, spv::Variable,
                         {pointer_to(spv::FunctionStorage, id_of(_p.instructions[i].type)), _instructions + u32(i),
                          spv::FunctionStorage});
        for (auto i = b.first; i + 1 < b.first + b.count; ++i)
        {
            line_before(i);
            instruction_of(i);
        }
        line_before(b.first + b.count - 1);
        terminator(b, _p.instructions[b.first + b.count - 1]);
    }
    emit(_code, spv::FunctionEnd, {});
}

void writer::write_control_entry()
{
    constexpr u32 k_scope_workgroup = 2;
    constexpr u32 k_scope_invocation = 4;
    auto bool_type = type_ref::none;
    for (auto i = isize(0); i < _p.types.size(); ++i)
        if (_p.types[i].kind == type_kind::bool_)
            bool_type = type_ref(i);

    name(_body, _p.name_at(_p.primitive.body_name_first, _p.primitive.body_name_size));
    auto const point = _bound++;
    auto const is_first = _bound++;
    auto const start = _bound++;
    auto const once = _bound++;
    auto const merge = _bound++;
    auto const ignored = _bound++;
    emit(_code, spv::Function, {id_of(_void), _function, 0u, _function_type});
    emit(_code, spv::Label, {start});
    emit(_code, spv::Load, {id_of(_uint), point, _invocation});
    // the k-th arrayed input is the k-th output: both are the one control point struct, member by member
    auto output = isize(0);
    for (auto i = isize(0); i < _p.stage_elements.size(); ++i)
    {
        auto const& e = _p.stage_elements[i];
        if (e.part != stage_part::input || e.array_size == 0)
            continue;
        while (_p.stage_elements[output].part != stage_part::output)
            ++output;
        auto const from = _bound++;
        auto const value = _bound++;
        auto const to = _bound++;
        emit(_code, spv::AccessChain, {_element_lanes[i], from, _element_variables[i], point});
        emit(_code, spv::Load, {id_of(e.type), value, from});
        emit(_code, spv::AccessChain, {_element_lanes[output], to, _element_variables[output], point});
        emit(_code, spv::Store, {to, value});
        ++output;
    }
    emit(_code, spv::ControlBarrier, {literal_id(k_scope_workgroup), literal_id(k_scope_invocation), literal_id(0)});
    emit(_code, spv::IEqual, {id_of(bool_type), is_first, point, literal_id(0)});
    emit(_code, spv::SelectionMerge, {merge, 0u});
    emit(_code, spv::BranchConditional, {is_first, once, merge});
    emit(_code, spv::Label, {once});
    emit(_code, spv::FunctionCall, {id_of(_void), ignored, _body});
    emit(_code, spv::Branch, {merge});
    emit(_code, spv::Label, {merge});
    emit(_code, spv::Return, {});
    emit(_code, spv::FunctionEnd, {});
}

cc::vector<u32> writer::assembled()
{
    auto out = cc::vector<u32>();
    out.push_back(0x07230203u);
    out.push_back(0x00010600u); // SPIR-V 1.6, which Vulkan 1.3 takes
    out.push_back(0x00530047u); // a generator number of our own, unregistered
    out.push_back(_bound);
    out.push_back(0);
    for (auto const c : _capabilities)
        emit(out, spv::Capability, {c});
    for (auto const e : _extensions)
    {
        head(out, spv::Extension, 1 + string_words(e));
        string_into(out, e);
    }
    if (_uses_glsl)
    {
        constexpr auto text = cc::string_view("GLSL.std.450");
        head(out, spv::ExtInstImport, 2 + string_words(text));
        out.push_back(_glsl);
        string_into(out, text);
    }
    emit(out, spv::MemoryModel, {0u, 1u}); // Logical, GLSL450
    out.push_back_range(_entry);
    out.push_back_range(_modes);
    out.push_back_range(_sources);
    out.push_back_range(_names);
    out.push_back_range(_annotations);
    out.push_back_range(_declarations);
    out.push_back_range(_code);
    return out;
}
