#include "llvm.hh"

#include <clean-core/math/bit.hh>

using namespace sgl;
using namespace sgl::ir::dxil::impl;

module_builder::module_builder()
{
    _constant_slots.resize_to_filled(1024, 0u);
    _void = add_type({.code = type_code::void_}, {});
    _i1 = add_type({.code = type_code::integer, .number = 1}, {});
    _i8 = add_type({.code = type_code::integer, .number = 8}, {});
    _i32 = add_type({.code = type_code::integer, .number = 32}, {});
    _i64 = add_type({.code = type_code::integer, .number = 64}, {});
    _f32 = add_type({.code = type_code::float_}, {});
    _label = add_type({.code = type_code::label}, {});
    _metadata_type = add_type({.code = type_code::metadata}, {});
    _attribute_lists.push_back({}); // list 0 is none
}

ll_type module_builder::add_type(type_entry const& t, cc::span<ll_type const> args)
{
    for (auto i = isize(0); i < _types.size(); ++i)
    {
        auto const& known = _types[i];
        if (known.code != t.code || known.number != t.number || known.element != t.element
            || isize(known.count) != args.size() || t.code == type_code::structure)
            continue;
        auto is_same = true;
        for (auto k = isize(0); k < args.size() && is_same; ++k)
            is_same = _type_args[known.first + u32(k)] == u32(args[k]);
        if (is_same)
            return ll_type(i);
    }
    auto entry = t;
    entry.first = u32(_type_args.size());
    entry.count = u32(args.size());
    for (auto const a : args)
        _type_args.push_back(u32(a));
    _types.push_back(entry);
    return ll_type(_types.size() - 1);
}

ll_type module_builder::integer(u32 width)
{
    return add_type({.code = type_code::integer, .number = width}, {});
}

ll_type module_builder::floating(u32 width)
{
    return add_type({.code = width == 16 ? type_code::half : width == 64 ? type_code::double_ : type_code::float_}, {});
}

ll_type module_builder::pointer(ll_type element, u32 address_space)
{
    return add_type({.code = type_code::pointer, .number = address_space, .element = element}, {});
}

ll_type module_builder::array(ll_type element, u64 count)
{
    return add_type({.code = type_code::array, .number = count, .element = element}, {});
}

ll_type module_builder::function(ll_type result, cc::span<ll_type const> params)
{
    auto args = cc::vector<ll_type>::create_with_capacity(params.size() + 1);
    args.push_back(result);
    for (auto const p : params)
        args.push_back(p);
    return add_type({.code = type_code::function}, args);
}

ll_type module_builder::structure(cc::string_view name, cc::span<ll_type const> members)
{
    for (auto i = isize(0); i < _types.size(); ++i)
        if (_types[i].code == type_code::structure && name_at(_types[i].name_first, _types[i].name_size) == name)
            return ll_type(i);
    return add_type({.code = type_code::structure, .name_first = add_name(name), .name_size = u32(name.size())}, members);
}

bool module_builder::is_float(ll_type t) const
{
    auto const code = _types[isize(t)].code;
    return code == type_code::half || code == type_code::float_ || code == type_code::double_;
}

u32 module_builder::bits_of(ll_type t) const
{
    auto const& info = _types[isize(t)];
    switch (info.code)
    {
    case type_code::integer:
        return u32(info.number);
    case type_code::half:
        return 16;
    case type_code::float_:
        return 32;
    case type_code::double_:
        return 64;
    default:
        return 0;
    }
}

u32 module_builder::add_name(cc::string_view name)
{
    auto const first = u32(_names.size());
    _names += name;
    return first;
}

cc::string_view module_builder::name_at(u32 first, u32 size) const
{
    return cc::string_view(_names).subview({.offset = isize(first), .size = isize(size)});
}

ll_value module_builder::add_constant(constant_entry const& c, cc::span<ll_value const> args)
{
    auto hash = (u32(c.type) * 0x9e3779b1u) ^ (u32(c.bits) * 0x85ebca6bu) ^ (u32(c.bits >> 32) * 0xc2b2ae35u)
              ^ (u32(c.code) * 0x27d4eb2fu);
    for (auto const a : args)
        hash = (hash ^ u32(a)) * 0x01000193u;
    auto const mask = _constant_slots.size() - 1;
    auto at = isize(hash) & mask;
    while (_constant_slots[at] != 0)
    {
        auto const index = _constant_slots[at] - 1;
        auto const& known = _constants[index];
        auto is_same
            = known.type == c.type && known.code == c.code && known.bits == c.bits && isize(known.count) == args.size();
        for (auto k = isize(0); k < args.size() && is_same; ++k)
            is_same = _constant_args[known.first + u32(k)] == u32(args[k]);
        if (is_same)
            return ll_value(index | k_ll_constant_bit);
        at = (at + 1) & mask;
    }
    auto entry = c;
    entry.first = u32(_constant_args.size());
    entry.count = u32(args.size());
    for (auto const a : args)
        _constant_args.push_back(u32(a));
    _constants.push_back(entry);
    _constant_slots[at] = u32(_constants.size());
    if (_constants.size() * 2 > _constant_slots.size())
    {
        // a rehash needs each one's hash again, so the table is rebuilt the way it was filled
        auto bigger = cc::vector<u32>::create_filled(_constant_slots.size() * 2, 0u);
        auto const wider = bigger.size() - 1;
        for (auto i = isize(0); i < _constants.size(); ++i)
        {
            auto const& held = _constants[i];
            auto h = (u32(held.type) * 0x9e3779b1u) ^ (u32(held.bits) * 0x85ebca6bu)
                   ^ (u32(held.bits >> 32) * 0xc2b2ae35u) ^ (u32(held.code) * 0x27d4eb2fu);
            for (auto k = u32(0); k < held.count; ++k)
                h = (h ^ _constant_args[held.first + k]) * 0x01000193u;
            auto slot = isize(h) & wider;
            while (bigger[slot] != 0)
                slot = (slot + 1) & wider;
            bigger[slot] = u32(i + 1);
        }
        _constant_slots = cc::move(bigger);
    }
    return ll_value(u32(_constants.size() - 1) | k_ll_constant_bit);
}

ll_value module_builder::constant_int(ll_type t, u64 bits)
{
    auto const width = bits_of(t);
    if (width < 64)
        bits &= (u64(1) << width) - 1;
    return add_constant({.type = t, .code = constant_code::integer, .bits = bits}, {});
}

ll_value module_builder::constant_float(ll_type t, u64 bits)
{
    return add_constant({.type = t, .code = constant_code::floating, .bits = bits}, {});
}

ll_value module_builder::constant_undef(ll_type t)
{
    return add_constant({.type = t, .code = constant_code::undefined}, {});
}

ll_value module_builder::constant_null(ll_type t)
{
    return add_constant({.type = t, .code = constant_code::null_}, {});
}

ll_value module_builder::constant_struct(ll_type t, cc::span<ll_value const> fields)
{
    return add_constant({.type = t, .code = constant_code::aggregate}, fields);
}

bool module_builder::constant_bits(ll_value v, u64& bits) const
{
    if (!ll_is_constant(v))
        return false;
    auto const& c = _constants[ll_index(v)];
    if (c.code != constant_code::integer && c.code != constant_code::floating)
        return false;
    bits = c.bits;
    return true;
}

u32 module_builder::attributes(cc::span<ll_attribute const> list)
{
    for (auto i = isize(1); i < _attribute_lists.size(); ++i)
    {
        auto const& known = _attribute_lists[i];
        if (isize(known.count) != list.size())
            continue;
        auto is_same = true;
        for (auto k = isize(0); k < list.size() && is_same; ++k)
        {
            auto const& held = _attribute_items[known.first + u32(k)];
            is_same = held.kind == list[k].kind && held.key == list[k].key && held.value == list[k].value;
        }
        if (is_same)
            return u32(i);
    }
    auto const first = u32(_attribute_items.size());
    for (auto const& a : list)
        _attribute_items.push_back(a);
    _attribute_lists.push_back({.first = first, .count = u32(list.size())});
    return u32(_attribute_lists.size() - 1);
}

ll_value module_builder::add_function(cc::string_view name, ll_type type, u32 attributes, bool is_declaration)
{
    _globals.push_back({.type = type,
                        .pointer = pointer(type),
                        .is_function = true,
                        .is_declaration = is_declaration,
                        .attributes = attributes,
                        .name_first = add_name(name),
                        .name_size = u32(name.size())});
    return ll_value(u32(_globals.size() - 1) | k_ll_global_bit);
}

ll_value module_builder::add_variable(cc::string_view name, ll_type value, u32 address_space, u32 alignment)
{
    _globals.push_back({.type = value,
                        .pointer = pointer(value, address_space),
                        .address_space = address_space,
                        .alignment = alignment,
                        .name_first = add_name(name),
                        .name_size = u32(name.size())});
    return ll_value(u32(_globals.size() - 1) | k_ll_global_bit);
}

u32 module_builder::md_string(cc::string_view text)
{
    _metadata.push_back({.code = md_code::string, .first = add_name(text), .count = u32(text.size())});
    return u32(_metadata.size());
}

u32 module_builder::md_value(ll_type t, ll_value v)
{
    _metadata.push_back({.code = md_code::value, .type = t, .value = v});
    return u32(_metadata.size());
}

u32 module_builder::md_node(cc::span<u32 const> operands)
{
    auto const first = u32(_md_args.size());
    for (auto const o : operands)
        _md_args.push_back(o);
    _metadata.push_back({.code = md_code::node, .first = first, .count = u32(operands.size())});
    return u32(_metadata.size());
}

void module_builder::md_named(cc::string_view name, cc::span<u32 const> nodes)
{
    auto const first = u32(_md_args.size());
    for (auto const n : nodes)
        _md_args.push_back(n);
    _named.push_back(
        {.name_first = add_name(name), .name_size = u32(name.size()), .first = first, .count = u32(nodes.size())});
}

void module_builder::begin_function()
{
    if (!_blocks.empty())
        _later_bodies.push_back(u32(_blocks.size()));
}

ll_block module_builder::add_block()
{
    _blocks.push_back({});
    return ll_block(_blocks.size() - 1);
}

void module_builder::open_block(ll_block b)
{
    _open = b;
    _blocks[isize(b)].first = u32(_instructions.size());
}

ll_value module_builder::push(inst_code code, ll_type type, cc::span<u32 const> args, u8 sub, u8 flags)
{
    auto const first = u32(_args.size());
    for (auto const a : args)
        _args.push_back(a);
    _instructions.push_back(
        {.code = code, .sub = sub, .flags = flags, .type = type, .first = first, .count = u32(args.size())});
    _blocks[isize(_open)].count = u32(_instructions.size()) - _blocks[isize(_open)].first;
    if (type == _void)
        return ll_value::none;
    _results.push_back(u32(_instructions.size() - 1));
    return ll_value(u32(_results.size() - 1));
}

ll_type module_builder::type_of(ll_value v) const
{
    if (ll_is_constant(v))
        return _constants[ll_index(v)].type;
    if (ll_is_global(v))
        return _globals[ll_index(v)].pointer;
    return _instructions[_results[ll_index(v)]].type;
}

ll_type module_builder::element_of(ll_type t) const
{
    return _types[isize(t)].element;
}

ll_value module_builder::folded_binary(binary_op op, ll_value a, ll_value b, u8 flags)
{
    auto x = u64(0);
    auto y = u64(0);
    auto const has_a = constant_bits(a, x);
    auto const has_b = constant_bits(b, y);
    if (!has_a && !has_b)
        return ll_value::none;
    auto const type = type_of(a);
    auto const width = bits_of(type);
    if (is_float(type))
    {
        // what an operation leaves of its other operand; a zero only where a signed zero may go
        constexpr auto one_32 = u64(0x3f800000u);
        if (width != 32)
            return ll_value::none;
        auto const no_sign = (flags & fp_flags::no_signed_zeros) != 0;
        if (op == binary_op::mul && has_b && y == one_32)
            return a;
        if (op == binary_op::mul && has_a && x == one_32)
            return b;
        if (op == binary_op::sdiv && has_b && y == one_32)
            return a;
        if (op == binary_op::add && has_a && x == 0 && no_sign)
            return b;
        if ((op == binary_op::add || op == binary_op::sub) && has_b && y == 0 && no_sign)
            return a;
        return ll_value::none;
    }
    if (has_a && has_b)
    {
        auto const mask = width >= 64 ? ~u64(0) : (u64(1) << width) - 1;
        auto const count = y & (width - 1);
        auto const sign = width >= 64 ? i64(x) : i64(x << (64 - width)) >> (64 - width);
        switch (op)
        {
        case binary_op::add:
            return constant_int(type, x + y);
        case binary_op::sub:
            return constant_int(type, x - y);
        case binary_op::mul:
            return constant_int(type, x * y);
        case binary_op::bit_and:
            return constant_int(type, x & y);
        case binary_op::bit_or:
            return constant_int(type, x | y);
        case binary_op::bit_xor:
            return constant_int(type, x ^ y);
        case binary_op::shl:
            return constant_int(type, x << count);
        case binary_op::lshr:
            return constant_int(type, (x & mask) >> count);
        case binary_op::ashr:
            return constant_int(type, u64(sign >> count));
        default:
            return ll_value::none;
        }
    }
    auto const known = has_a ? x : y;
    auto const other = has_a ? b : a;
    auto const is_right = has_b;
    switch (op)
    {
    case binary_op::add:
    case binary_op::bit_or:
    case binary_op::bit_xor:
        return known == 0 ? other : ll_value::none;
    case binary_op::sub:
    case binary_op::shl:
    case binary_op::lshr:
    case binary_op::ashr:
        return is_right && known == 0 ? other : ll_value::none;
    case binary_op::mul:
        return known == 1 ? other : ll_value::none;
    case binary_op::bit_and:
        if (width == 1)
            return known != 0 ? other : (has_a ? a : b);
        return ll_value::none;
    default:
        return ll_value::none;
    }
}

ll_value module_builder::binary(binary_op op, ll_value a, ll_value b, u8 flags)
{
    if (auto const folded = folded_binary(op, a, b, flags); folded != ll_value::none)
        return folded;
    u32 const args[] = {u32(a), u32(b)};
    return push(inst_code::binary, type_of(a), args, u8(op), flags);
}

ll_value module_builder::cast(cast_op op, ll_value a, ll_type to)
{
    auto x = u64(0);
    if ((op == cast_op::sitofp || op == cast_op::uitofp) && bits_of(to) == 32 && constant_bits(a, x))
    {
        auto const width = bits_of(type_of(a));
        if (width <= 32)
        {
            auto const sign = i64(x << (64 - width)) >> (64 - width);
            auto const value = op == cast_op::sitofp ? f32(sign) : f32(x);
            return constant_float(to, cc::bit_cast<u32>(value));
        }
    }
    u32 const args[] = {u32(a)};
    return push(inst_code::cast, to, args, u8(op));
}

ll_value module_builder::compare(predicate p, ll_value a, ll_value b, u8 flags)
{
    auto x = u64(0);
    auto y = u64(0);
    if (u8(p) >= u8(predicate::i_eq) && constant_bits(a, x) && constant_bits(b, y))
    {
        auto const width = bits_of(type_of(a));
        auto const mask = width >= 64 ? ~u64(0) : (u64(1) << width) - 1;
        auto const ux = x & mask;
        auto const uy = y & mask;
        auto const sx = width >= 64 ? i64(x) : i64(x << (64 - width)) >> (64 - width);
        auto const sy = width >= 64 ? i64(y) : i64(y << (64 - width)) >> (64 - width);
        switch (p)
        {
        case predicate::i_eq:
            return constant_bool(ux == uy);
        case predicate::i_ne:
            return constant_bool(ux != uy);
        case predicate::i_ugt:
            return constant_bool(ux > uy);
        case predicate::i_uge:
            return constant_bool(ux >= uy);
        case predicate::i_ult:
            return constant_bool(ux < uy);
        case predicate::i_ule:
            return constant_bool(ux <= uy);
        case predicate::i_sgt:
            return constant_bool(sx > sy);
        case predicate::i_sge:
            return constant_bool(sx >= sy);
        case predicate::i_slt:
            return constant_bool(sx < sy);
        case predicate::i_sle:
            return constant_bool(sx <= sy);
        default:
            break;
        }
    }
    u32 const args[] = {u32(a), u32(b)};
    return push(inst_code::compare, _i1, args, u8(p), flags);
}

ll_value module_builder::select(ll_value condition, ll_value yes, ll_value no)
{
    if (yes == no)
        return yes;
    if (auto x = u64(0); constant_bits(condition, x))
        return x != 0 ? yes : no;
    u32 const args[] = {u32(yes), u32(no), u32(condition)};
    return push(inst_code::select, type_of(yes), args);
}

ll_value module_builder::extract_value(ll_value aggregate, u32 index)
{
    auto const& t = _types[isize(type_of(aggregate))];
    u32 const args[] = {u32(aggregate), index};
    return push(inst_code::extract_value, ll_type(_type_args[t.first + index]), args);
}

ll_value module_builder::call(ll_value callee, ll_type type, cc::span<ll_value const> args, bool is_stable)
{
    auto raw = cc::vector<u32>::create_with_capacity(args.size() + 1);
    raw.push_back(u32(callee));
    for (auto const a : args)
        raw.push_back(u32(a));
    ++_call_count;
    return push(inst_code::call, ll_type(_type_args[_types[isize(type)].first]), raw, 0, is_stable ? 1 : 0);
}

ll_value module_builder::alloca_of(ll_type allocated, u32 alignment)
{
    auto const size = constant_i32(1);
    u32 const args[] = {u32(allocated), u32(size)};
    return push(inst_code::alloca_, pointer(allocated), args, 0, u8(log2_plus_one(alignment)));
}

ll_value module_builder::load(ll_type t, ll_value pointer, u32 alignment)
{
    u32 const args[] = {u32(pointer)};
    return push(inst_code::load, t, args, 0, u8(log2_plus_one(alignment)));
}

void module_builder::store(ll_value pointer, ll_value value, u32 alignment)
{
    u32 const args[] = {u32(pointer), u32(value)};
    (void)push(inst_code::store, _void, args, 0, u8(log2_plus_one(alignment)));
}

ll_value module_builder::element_pointer(ll_type source, ll_value base, cc::span<ll_value const> indices)
{
    auto raw = cc::vector<u32>::create_with_capacity(indices.size() + 2);
    raw.push_back(u32(source));
    raw.push_back(u32(base));
    auto element = source;
    for (auto i = isize(0); i < indices.size(); ++i)
    {
        raw.push_back(u32(indices[i]));
        if (i > 0)
            element = element_of(element);
    }
    auto const space = u32(_types[isize(type_of(base))].number);
    return push(inst_code::element_pointer, pointer(element, space), raw);
}

ll_value module_builder::atomic_rmw(rmw_op op, ll_value pointer, ll_value value)
{
    u32 const args[] = {u32(pointer), u32(value)};
    return push(inst_code::atomic_rmw, type_of(value), args, u8(op));
}

ll_value module_builder::atomic_exchange(ll_value pointer, ll_value compare, ll_value desired)
{
    u32 const args[] = {u32(pointer), u32(compare), u32(desired)};
    ll_type const members[] = {type_of(compare), _i1};
    // LLVM's result is the pair, which no name of ours needs
    auto const pair = add_type({.code = type_code::structure}, members);
    return push(inst_code::atomic_exchange, pair, args);
}

ll_value module_builder::phi(ll_type t, isize edges)
{
    auto raw = cc::vector<u32>::create_filled(edges * 2, 0u);
    return push(inst_code::phi, t, raw);
}

void module_builder::set_phi_edge(ll_value phi, isize edge, ll_value value, ll_block from)
{
    auto const& inst = _instructions[_results[ll_index(phi)]];
    _args[inst.first + u32(edge) * 2] = u32(value);
    _args[inst.first + u32(edge) * 2 + 1] = u32(from);
}

void module_builder::branch(ll_block to)
{
    u32 const args[] = {u32(to)};
    (void)push(inst_code::branch, _void, args);
}

void module_builder::branch_if(ll_value condition, ll_block yes, ll_block no)
{
    u32 const args[] = {u32(yes), u32(no), u32(condition)};
    (void)push(inst_code::branch_if, _void, args);
}

void module_builder::switch_on(ll_value selector, ll_block fallback, cc::span<u32 const> cases)
{
    auto raw = cc::vector<u32>::create_with_capacity(cases.size() + 2);
    raw.push_back(u32(selector));
    raw.push_back(u32(fallback));
    for (auto const c : cases)
        raw.push_back(c);
    (void)push(inst_code::switch_, _void, raw);
}

void module_builder::return_void()
{
    (void)push(inst_code::return_void, _void, {});
}

void module_builder::unreachable()
{
    (void)push(inst_code::unreachable, _void, {});
}
