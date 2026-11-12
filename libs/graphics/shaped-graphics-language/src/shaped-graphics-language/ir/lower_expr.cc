#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ir/impl/lowering.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::ir;
using namespace sgl::ir::impl;

value_ref lowering::constant_of(type_ref type, u32 low, u32 high)
{
    auto hash = (u32(type) * 0x9e3779b1u) ^ (low * 0x85ebca6bu) ^ (high * 0xc2b2ae35u);
    auto const mask = isize(_constant_slots.size()) - 1;
    auto at = isize(hash) & mask;
    while (_constant_slots[at] != 0)
    {
        auto const index = _constant_slots[at] - 1;
        auto const& c = _out.constants[index];
        if (c.type == type && c.low == low && c.high == high && !c.is_null && !c.is_undefined)
            return constant_at(index);
        at = (at + 1) & mask;
    }
    _out.constants.push_back({.type = type, .low = low, .high = high});
    _constant_slots[at] = u32(_out.constants.size());
    // rehashed at half full, which keeps a probe short
    if (_out.constants.size() * 2 > _constant_slots.size())
    {
        auto bigger = cc::vector<u32>::create_filled(_constant_slots.size() * 2, 0u);
        auto const wider = bigger.size() - 1;
        for (auto i = isize(0); i < _out.constants.size(); ++i)
        {
            auto const& c = _out.constants[i];
            if (c.is_null || c.is_undefined)
                continue;
            auto slot = isize((u32(c.type) * 0x9e3779b1u) ^ (c.low * 0x85ebca6bu) ^ (c.high * 0xc2b2ae35u)) & wider;
            while (bigger[slot] != 0)
                slot = (slot + 1) & wider;
            bigger[slot] = u32(i + 1);
        }
        _constant_slots = cc::move(bigger);
    }
    return constant_at(u32(_out.constants.size() - 1));
}

value_ref lowering::null_of(type_ref type)
{
    _out.constants.push_back({.type = type, .is_null = true});
    return constant_at(u32(_out.constants.size() - 1));
}

value_ref lowering::int_constant(i32 value)
{
    return constant_of(_int, u32(value));
}

value_ref lowering::uint_constant(u32 value)
{
    return constant_of(_uint, value);
}

value_ref lowering::bool_constant(bool value)
{
    return constant_of(_bool, value ? 1u : 0u);
}

value_ref lowering::float_constant(type_ref type, f64 value)
{
    auto const width = _out.at(type).width;
    if (width == 64)
    {
        auto const bits = cc::bit_cast<u64>(value);
        return constant_of(type, u32(bits), u32(bits >> 32));
    }
    if (width == 32)
        return constant_of(type, cc::bit_cast<u32>(f32(value)));

    // binary16 from binary32, rounding to nearest; a shader's half literal is short, so a tie is rare and exact
    auto const bits = cc::bit_cast<u32>(f32(value));
    auto const sign = (bits >> 16) & 0x8000u;
    auto const exponent = i32((bits >> 23) & 0xffu) - 127 + 15;
    auto mantissa = bits & 0x7fffffu;
    if (((bits >> 23) & 0xffu) == 0xffu)
        return constant_of(type, sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
    if (exponent >= 31)
        return constant_of(type, sign | 0x7c00u);
    if (exponent <= 0)
    {
        if (exponent < -10)
            return constant_of(type, sign);
        mantissa |= 0x800000u;
        auto const shift = u32(14 - exponent);
        return constant_of(type, sign | ((mantissa + (1u << (shift - 1))) >> shift));
    }
    auto const packed = (u32(exponent) << 10) | (mantissa >> 13);
    return constant_of(type, sign | (packed + ((mantissa >> 12) & 1u)));
}

value_ref lowering::literal(flat_expr const& x)
{
    auto const type = value_type(x.type);
    if (auto const* const l = x.node.try_as<flat_literal>())
        return float_constant(type, l->value);
    if (auto const* const l = x.node.try_as<flat_int_literal>())
    {
        auto const t = _out.at(type);
        if (t.kind == type_kind::float_)
            return float_constant(type, f64(l->value));
        if (t.width == 64)
            return constant_of(type, u32(l->value), l->value < 0 && !l->is_unsigned ? 0xffffffffu : 0u);
        if (t.width == 16)
            return constant_of(type, t.is_signed ? u32(i32(i16(l->value))) : u32(u16(l->value)));
        return constant_of(type, u32(l->value));
    }
    if (auto const* const l = x.node.try_as<flat_wide_literal>())
        return constant_of(type, u32(l->bits), u32(l->bits >> 32));
    if (auto const* const l = x.node.try_as<flat_bool_literal>())
        return bool_constant(l->value);
    return value_ref::none;
}

i32 lowering::position_in(type_id object, i32 member) const
{
    if (_m.builtin_type_of(object) != nullptr)
        return member;
    // a void member takes no place, so the ones after it move up
    auto const members = _m.at(_m.at(object).members);
    auto position = 0;
    for (auto i = 0; i < member; ++i)
        position += members[i].type == checked_module::void_type ? 0 : 1;
    return position;
}

value_ref lowering::extracted(value_ref composite, type_ref type, u32 index)
{
    auto const t = _out.at(_out.type_of(composite));
    if (t.kind != type_kind::vector && t.kind != type_kind::structure && t.kind != type_kind::array
        && t.kind != type_kind::matrix)
        return composite;
    u32 const operands[] = {u32(composite), index};
    return add(op::extract, type, operands);
}

value_ref lowering::member_of(flat_expr const& x, flat_member const& m)
{
    auto const object = expr(m.object);
    if (!m.is_swizzle())
        return extracted(object, value_type(x.type), u32(position_in(_e.at(m.object).type, m.member)));
    auto operands = cc::vector<u32>();
    operands.push_back(u32(object));
    operands.push_back(u32(object));
    for (auto i = 0; i < m.letters.count; ++i)
        operands.push_back(u32(m.letters.fields[i]));
    return add(op::shuffle, value_type(x.type), operands);
}

value_ref lowering::splat(value_ref scalar, type_ref vector)
{
    auto const t = _out.at(vector);
    if (t.kind != type_kind::vector)
        return scalar;
    u32 const operands[] = {u32(scalar), u32(scalar), u32(scalar), u32(scalar)};
    return add(op::construct, vector, cc::span<u32 const>(operands, isize(t.count)));
}

/// Whether `id` may run although the operand it stands right of decided otherwise: nothing it does can go wrong.
bool lowering::is_safe_early(flat_expr_id id) const
{
    auto const& x = _e.at(id);
    if (x.node.is<flat_literal>() || x.node.is<flat_int_literal>() || x.node.is<flat_bool_literal>()
        || x.node.is<flat_wide_literal>() || x.node.is<flat_enum_value>() || x.node.is<flat_local_ref>())
        return true;
    if (auto const* const m = x.node.try_as<flat_member>())
        return is_safe_early(m->object);
    if (auto const* const n = x.node.try_as<flat_not>())
        return is_safe_early(n->operand);
    if (auto const* const a = x.node.try_as<flat_and>())
        return is_safe_early(a->lhs) && is_safe_early(a->rhs);
    if (auto const* const o = x.node.try_as<flat_or>())
        return is_safe_early(o->lhs) && is_safe_early(o->rhs);
    if (auto const* const c = x.node.try_as<flat_call>())
    {
        // a comparison over what is safe; a division can trap and a read can miss
        auto const* const record = _m.builtin_function(c->intrinsic);
        if (record == nullptr || !c->is_pure || c->arguments.count != 2)
            return false;
        auto const name = cc::string_view(record->name);
        if (!name.starts_with("less") && !name.starts_with("greater") && !name.starts_with("same")
            && !name.starts_with("differs"))
            return false;
        for (auto const a : _e.at(c->arguments))
            if (!is_safe_early(a))
                return false;
        return true;
    }
    return false;
}

value_ref lowering::short_circuit(flat_expr_id lhs, flat_expr_id rhs, bool is_and)
{
    auto const left = expr(lhs);
    if (is_safe_early(rhs))
        return add(is_and ? op::logical_and : op::logical_or, _bool, {left, expr(rhs)});

    auto const from = _open;
    auto const taken = new_block();
    auto const merge = new_block();
    set_merge(merge_kind::selection, merge);
    if (is_and)
        branch_if(left, taken, merge);
    else
        branch_if(left, merge, taken);

    open_block(taken);
    auto const right = expr(rhs);
    auto const reached = _open;
    branch(merge);

    open_block(merge);
    u32 const operands[] = {u32(left), u32(from), u32(right), u32(reached)};
    return add(op::phi, _bool, operands);
}

value_ref lowering::expr(flat_expr_id id)
{
    auto const& x = _e.at(id);
    auto result = value_ref::none;
    x.node.visit(
        [&](flat_invalid const&) { unsupported("an unfilled node"); },
        [&](flat_literal const&) { result = literal(x); }, //
        [&](flat_int_literal const&) { result = literal(x); }, [&](flat_wide_literal const&) { result = literal(x); },
        [&](flat_bool_literal const&) { result = literal(x); },
        [&](flat_enum_value const& v) { result = int_constant(_m.at(_m.at(x.type).cases)[v.case_index].value); },
        [&](flat_local_ref const& l)
        {
            if (auto const first = stage_array_of(id); first != k_no_element)
            {
                result = read_stage_array(x.type, first);
                return;
            }
            auto const position = index_of(l.local);
            result = _locals[position];
            if (_is_variable[position])
                result = load({.pointer = result, .type = x.type});
        },
        [&](flat_binding_member const&)
        {
            if (auto const memory = place_of(id); memory.has_value())
                result = load(memory.value());
            else
                unsupported("a resource used as a value");
        },
        [&](flat_file_sampler const&) { unsupported("a file sampler used as a value"); },
        [&](flat_written_word const&) { unsupported("a word of the workgroup shadow used as a value"); },
        [&](flat_buffer_element const&)
        {
            if (auto const memory = place_of(id); memory.has_value())
                result = load(memory.value());
            else
                unsupported("a buffer element no place reaches");
        },
        [&](flat_buffer_length const& b)
        {
            auto const* const bound = _e.at(b.buffer).node.try_as<flat_binding_member>();
            auto length = bound == nullptr ? uniform_element_length(b.buffer) : value_ref::none;
            if (!ir::is_valid(length))
            {
                auto const memory = bound != nullptr
                                      ? bound_resource(emit::impl::resource_of(_plan, bound->binding, bound->member))
                                      : bound_element(b.buffer);
                if (!ir::is_valid(memory))
                    return unsupported("the length of a binding array's element");
                u32 const operands[] = {u32(memory), 0u};
                length = add(op::array_length, _uint, operands);
            }
            if (_m.at(_e.at(b.buffer).type).kind == check::type_kind::bytes)
                length = add(op::imul, _uint, {length, uint_constant(4)});
            auto const wanted = value_type(x.type);
            result = wanted == _uint ? length : add(op::bitcast, wanted, {length});
        },
        [&](flat_element const& a)
        {
            // EMIT-240: one vertex of a parameter is its elements at that index, whatever the index is
            if (auto element = stage_array_of(a.object); element != k_no_element)
            {
                result = read_struct(x.type, element, expr(a.index));
                return;
            }
            if (auto const memory = place_of(id); memory.has_value())
            {
                result = load(memory.value());
                return;
            }
            auto const object = expr(a.object);
            auto const type = value_type(x.type);
            if (auto const* const k = _e.at(a.index).node.try_as<flat_int_literal>())
            {
                result = extracted(object, type, u32(k->value));
                return;
            }
            // an index that is no constant reaches an array in a variable alone
            auto const variable = spilled(object, _e.at(a.object).type);
            u32 const chain[] = {u32(variable), u32(expr(a.index))};
            auto const pointer = add(op::access_chain, pointer_type(type, storage::function), chain);
            result = add(op::load, type, {pointer});
        },
        [&](flat_member const& m)
        {
            // a place keeps a read of one field from loading the whole value around it
            if (!m.is_swizzle())
                if (auto const memory = place_of(id); memory.has_value())
                {
                    result = load(memory.value());
                    return;
                }
            result = member_of(x, m);
        },
        [&](flat_construct const& c)
        {
            auto operands = cc::vector<u32>();
            for (auto const argument : _e.at(c.arguments))
                if (_e.at(argument).type != checked_module::void_type)
                    operands.push_back(u32(expr(argument)));
            result = add(op::construct, value_type(x.type), operands);
        },
        [&](flat_call const& c) { result = call(x, c); },
        [&](flat_not const& n) { result = add(op::logical_not, _bool, {expr(n.operand)}); },
        [&](flat_and const& a) { result = short_circuit(a.lhs, a.rhs, true); },
        [&](flat_or const& o) { result = short_circuit(o.lhs, o.rhs, false); },
        [&](flat_block const&) { unsupported("a block in a core tree"); },
        [&](flat_by_target const&) { unsupported("a trace's two forms in a core tree"); });
    return result;
}
