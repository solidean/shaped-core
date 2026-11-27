#include "writer.hh"

#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil::impl;

// Three kinds of place, each with its own addressing.
// A variable and workgroup memory are arrays of scalars, the columns `memory` describes, each leaf at a position in one.
// A block of constants is read in rows of 16 bytes, and a buffer an element and a byte offset at a time.

namespace
{
/// The address space DXIL gives workgroup memory.
constexpr u32 k_group_shared = 3;

/// Which of a memory's arrays holds a scalar of this LLVM width and class: bools live as i32.
[[nodiscard]] constexpr isize storage_slot(bool is_float, u32 bits)
{
    if (is_float)
        return bits == 16 ? 0 : bits == 64 ? 2 : 1;
    return bits == 16 ? 3 : bits == 64 ? 5 : 4;
}
} // namespace

bool writer::holds_composite_array(type_ref t)
{
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::array:
    {
        auto const element = _p.at(info.element).kind;
        if (element == type_kind::structure || element == type_kind::vector || element == type_kind::matrix)
            return true;
        return holds_composite_array(info.element);
    }
    case type_kind::structure:
        for (auto m = u32(0); m < info.member_count; ++m)
            if (holds_composite_array(_p.members[info.first_member + m].type))
                return true;
        return false;
    default:
        return false;
    }
}

void writer::lay_out_split(memory& entry,
                           type_ref t,
                           cc::vector<u32>& key,
                           u64 position,
                           bool whole_lanes,
                           cc::vector<cc::vector<u32>>& keys,
                           cc::vector<column_shape>& shapes)
{
    auto const& info = _p.at(t);
    auto const is_key = [&](cc::vector<u32> const& other)
    {
        if (other.size() != key.size())
            return false;
        for (auto k = isize(0); k < key.size(); ++k)
            if (other[k] != key[k])
                return false;
        return true;
    };
    auto const leaf = [&](u64 at)
    {
        // the latest column first, since a struct's leaves meet their columns in order
        auto column = u32(keys.size());
        for (auto c = isize(keys.size()); c-- > 0;)
            if (is_key(keys[c]))
            {
                column = u32(c);
                break;
            }
        if (column == keys.size())
        {
            keys.push_back(key);
            shapes.push_back({.suffix = column});
        }
        entry.column_of.push_back(column);
        entry.position_of.push_back(u32(at));
        shapes[column].size = cc::max(shapes[column].size, u32(at) + 1);
    };
    switch (info.kind)
    {
    case type_kind::structure:
        for (auto m = u32(0); m < info.member_count; ++m)
        {
            key.push_back(m);
            lay_out_split(entry, _p.members[info.first_member + m].type, key, position, whole_lanes, keys, shapes);
            key.pop_back();
        }
        return;
    case type_kind::array:
        for (auto e = u32(0); e < info.count; ++e)
            lay_out_split(entry, info.element, key, position * info.count + e, whole_lanes, keys, shapes);
        return;
    case type_kind::vector:
    case type_kind::matrix:
        if (!whole_lanes)
        {
            // a lane is a field of its own, and so is a matrix's column
            for (auto k = u32(0); k < info.count; ++k)
            {
                key.push_back(k);
                lay_out_split(entry, info.element, key, position, whole_lanes, keys, shapes);
                key.pop_back();
            }
            return;
        }
        [[fallthrough]];
    default:
    {
        auto const count = leaves_of(t);
        for (auto k = u32(0); k < count; ++k)
            leaf(position * count + k);
        return;
    }
    }
}

cc::vector<writer::column_shape> writer::lay_out_memory(memory& entry, bool whole_lanes)
{
    auto shapes = cc::vector<column_shape>();
    // AMD's compiler crashes on such an array held flat: docs/bugs-external/amd-dxil-flat-local-array-crashes-compiler
    if (holds_composite_array(entry.value))
    {
        auto key = cc::vector<u32>();
        auto keys = cc::vector<cc::vector<u32>>();
        lay_out_split(entry, entry.value, key, 0, whole_lanes, keys, shapes);
    }
    else
    {
        // one column per scalar type, each as long as the value, so a leaf's position is the leaf
        i32 slots[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        for (auto l = u32(0); l < entry.leaves; ++l)
        {
            auto type = scalar_type(scalar_at(entry.value, l));
            if (type == _m.bool_type())
                type = _m.int_type();
            auto const slot = storage_slot(_m.is_float(type), _m.bits_of(type));
            if (slots[slot] < 0)
            {
                slots[slot] = i32(shapes.size());
                shapes.push_back({.size = entry.leaves, .suffix = u32(slot)});
            }
            entry.column_of.push_back(u32(slots[slot]));
            entry.position_of.push_back(l);
        }
    }
    for (auto l = u32(0); l < entry.leaves; ++l)
    {
        auto& shape = shapes[isize(entry.column_of[isize(l)])];
        if (shape.type != ll_type::none)
            continue;
        auto type = scalar_type(scalar_at(entry.value, l));
        shape.type = type == _m.bool_type() ? _m.int_type() : type;
    }
    return shapes;
}

u32 writer::memory_of_type(type_ref value, bool is_workgroup, cc::string_view name, bool whole_lanes)
{
    auto entry = memory{.value = value, .leaves = leaves_of(value)};
    for (auto const& shape : lay_out_memory(entry, whole_lanes))
    {
        auto const array = _m.array(shape.type, shape.size);
        auto const alignment = _m.bits_of(shape.type) / 8;
        if (is_workgroup)
            _summary.workgroup_bytes += shape.size * alignment;
        entry.columns.push_back(
            is_workgroup ? _m.add_variable(cc::format("{}.{}", name, shape.suffix), array, k_group_shared, alignment)
                         : _m.alloca_of(array, alignment));
    }
    _memories.push_back(cc::move(entry));
    return u32(_memories.size() - 1);
}

u32 writer::memory_of_table(global const& g, bool whole_lanes)
{
    auto const value = _p.at(g.type).element;
    auto entry = memory{.value = value, .leaves = leaves_of(value)};
    auto const name = _p.name_at(g.name_first, g.name_size);
    auto const shapes = lay_out_memory(entry, whole_lanes);
    for (auto c = isize(0); c < shapes.size(); ++c)
    {
        auto const type = shapes[c].type;
        // a position no leaf of the column takes holds zero
        auto elements = cc::vector<ll_value>();
        for (auto k = u32(0); k < shapes[c].size; ++k)
            elements.push_back(_m.is_float(type) ? _m.constant_float(type, 0) : _m.constant_int(type, 0));
        for (auto k = u32(0); k < entry.leaves; ++k)
        {
            if (entry.column_of[isize(k)] != u32(c))
                continue;
            auto const leaf = value_ref(_p.initializers[isize(g.initializer_first) + k]);
            auto const& constant = _p.constants[index_of(leaf)];
            auto const bits = constant.is_null ? u64(0) : (u64(constant.high) << 32) | constant.low;
            elements[isize(entry.position_of[isize(k)])]
                = _m.is_float(type) ? _m.constant_float(type, bits) : _m.constant_int(type, bits);
        }
        auto const array = _m.array(type, shapes[c].size);
        entry.columns.push_back(_m.add_constant_table(cc::format("{}.{}", name, shapes[c].suffix), array,
                                                      _m.constant_struct(array, elements), _m.bits_of(type) / 8));
    }
    _memories.push_back(cc::move(entry));
    return u32(_memories.size() - 1);
}

void writer::declare_memory()
{
    _local_memory.resize_to_filled(_p.locals.size(), -1);
    _global_memory.resize_to_filled(_p.globals.size(), -1);

    // a variable indexed into a vector or a matrix at run time keeps each whole, since its lanes cannot be columns
    auto local_lanes = cc::vector<u8>();
    local_lanes.resize_to_filled(_p.locals.size(), 0);
    auto global_lanes = cc::vector<u8>();
    global_lanes.resize_to_filled(_p.globals.size(), 0);
    auto roots = cc::vector<value_ref>();
    roots.resize_to_filled(_p.instructions.size(), value_ref::none);
    for (auto pass = 0; pass < 2; ++pass)
        for (auto n = isize(0); n < _p.instructions.size(); ++n)
        {
            auto const& i = _p.instructions[n];
            if (i.code != op::access_chain)
                continue;
            auto const operands = _p.operands_of(i);
            auto const base = value_ref(operands[0]);
            auto const root = is_operation(base) ? roots[index_of(base)] : base;
            auto const is_memory_global = [&]
            {
                auto const space = _p.at(_p.globals[index_of(root)].type).space;
                return space == storage::workgroup || space == storage::table;
            };
            if (!is_local(root) && !(is_global(root) && is_memory_global()))
                continue;
            roots[n] = root;
            auto type = _p.at(_p.type_of(base)).element;
            for (auto k = isize(1); k < operands.size(); ++k)
            {
                auto const& info = _p.at(type);
                auto const at = value_ref(operands[k]);
                if (info.kind == type_kind::structure)
                {
                    type = _p.members[info.first_member + _p.constants[index_of(at)].low].type;
                    continue;
                }
                if ((info.kind == type_kind::vector || info.kind == type_kind::matrix) && !is_constant(at))
                    (is_local(root) ? local_lanes[index_of(root)] : global_lanes[index_of(root)]) = 1;
                type = info.element;
            }
        }

    for (auto l = isize(0); l < _p.locals.size(); ++l)
    {
        auto const value = _p.at(_p.locals[l].type).element;
        _local_memory[l] = i32(memory_of_type(value, false, {}, local_lanes[l] != 0));
    }
    for (auto g = isize(0); g < _p.globals.size(); ++g)
    {
        auto const& pointer = _p.at(_p.globals[g].type);
        if (pointer.space == storage::table)
        {
            _global_memory[g] = i32(memory_of_table(_p.globals[g], global_lanes[g] != 0));
            continue;
        }
        if (pointer.space == storage::workgroup)
            _global_memory[g] = i32(memory_of_type(pointer.element, true,
                                                   _p.name_at(_p.globals[g].name_first, _p.globals[g].name_size),
                                                   global_lanes[g] != 0));
        if (pointer.space != storage::task_payload)
            continue;
        auto alignment = u32(1);
        _summary.payload_bytes = payload_layout(pointer.element, alignment);
        _payload_type = payload_type(pointer.element);
        _payload_roots.resize_to_filled(_p.globals.size(), {});
        // a task stage holds it in workgroup memory, and a mesh stage asks for what it was handed
        if (_p.stage == check::stage::task)
            _payload = _m.add_variable("sgl_payload", _payload_type, k_group_shared, alignment);
        else
        {
            ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::get_mesh_payload)].number)};
            _payload = call(dx_op::get_mesh_payload, _payload_type, args);
        }
        _payload_roots[g] = {.pointer = _payload, .type = _payload_type};
    }
}

ll_type writer::payload_type(type_ref t)
{
    auto const& info = _p.at(t);
    auto const scalar_of = [&](type_ref scalar)
    {
        auto const held = scalar_type(scalar);
        return held == _m.bool_type() ? _m.int_type() : held;
    };
    switch (info.kind)
    {
    case type_kind::vector:
        return _m.array(scalar_of(info.element), info.count);
    case type_kind::matrix:
        return _m.array(scalar_of(_p.at(info.element).element), u64(info.count) * _p.at(info.element).count);
    case type_kind::array:
        return _m.array(payload_type(info.element), info.count);
    case type_kind::structure:
    {
        auto members = cc::vector<ll_type>();
        for (auto m = u32(0); m < info.member_count; ++m)
            members.push_back(payload_type(_p.members[info.first_member + m].type));
        return _m.structure(cc::format("struct.{}", _p.name_at(info.name_first, info.name_size)), members);
    }
    default:
        return scalar_of(t);
    }
}

u32 writer::payload_layout(type_ref t, u32& alignment)
{
    auto const& info = _p.at(t);
    auto const element_bytes = [&](type_ref element, u32 count)
    {
        auto held = u32(1);
        auto const size = payload_layout(element, held);
        alignment = cc::max(alignment, held);
        return size * count;
    };
    switch (info.kind)
    {
    case type_kind::vector:
    case type_kind::array:
    case type_kind::matrix:
        return element_bytes(info.element, info.count);
    case type_kind::structure:
    {
        auto at = u32(0);
        auto most = u32(1);
        for (auto m = u32(0); m < info.member_count; ++m)
        {
            auto held = u32(1);
            auto const size = payload_layout(_p.members[info.first_member + m].type, held);
            at = (at + held - 1) / held * held + size;
            most = cc::max(most, held);
        }
        alignment = cc::max(alignment, most);
        return (at + most - 1) / most * most;
    }
    default:
    {
        auto const bytes = info.kind == type_kind::bool_ ? u32(4) : u32(info.width) / 8;
        alignment = cc::max(alignment, bytes);
        return bytes;
    }
    }
}

ll_value writer::index_sum(ll_value a, ll_value b)
{
    auto x = u64(0);
    auto y = u64(0);
    if (_m.constant_bits(a, x) && _m.constant_bits(b, y))
        return _m.constant_i32(u32(x + y));
    return _m.binary(binary_op::add, a, b);
}

void writer::payload_chain(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto result = place_of(value_ref(operands[0]));
    auto type = _p.at(_p.type_of(value_ref(operands[0]))).element;
    auto path = cc::vector<ll_value>::create_copy_of(
        cc::span<ll_value const>(_paths.data() + result.path_first, isize(result.path_count)));
    for (auto k = isize(1); k < operands.size(); ++k)
    {
        auto const value = value_ref(operands[k]);
        auto const& info = _p.at(type);
        if (info.kind == type_kind::structure)
        {
            auto const member = _p.constants[index_of(value)].low;
            path.push_back(_m.constant_i32(member));
            type = _p.members[info.first_member + member].type;
            continue;
        }
        auto const at = leaf_at(value);
        if (info.kind == type_kind::matrix)
        {
            auto const rows = _p.at(info.element).count;
            auto known = u64(0);
            path.push_back(_m.constant_bits(at, known) ? _m.constant_i32(u32(known) * rows)
                                                       : _m.binary(binary_op::mul, at, _m.constant_i32(rows)));
            result.is_column = true;
        }
        else if (info.kind == type_kind::vector && result.is_column)
        {
            path.back() = index_sum(path.back(), at);
            result.is_column = false;
        }
        else
            path.push_back(at);
        type = info.element;
    }
    result.path_first = u32(_paths.size());
    result.path_count = u32(path.size());
    _paths.push_back_range(path);
    _places[index] = result;
}

void writer::payload_pointers(type_ref t, cc::vector<ll_value>& path, bool is_column, cc::vector<ll_value>& out)
{
    auto const& info = _p.at(t);
    auto const each = [&](u32 count, auto&& visit)
    {
        for (auto k = u32(0); k < count; ++k)
        {
            path.push_back(_m.constant_i32(k));
            visit(k);
            path.pop_back();
        }
    };
    auto const leaf = [&]
    {
        auto indices = cc::vector<ll_value>();
        indices.push_back(_m.constant_i32(0));
        indices.push_back_range(path);
        out.push_back(_m.element_pointer(_reached->type, _reached->pointer, indices));
    };
    switch (info.kind)
    {
    case type_kind::structure:
        return each(info.member_count,
                    [&](u32 m) { payload_pointers(_p.members[info.first_member + m].type, path, false, out); });
    case type_kind::array:
        return each(info.count, [&](u32) { payload_pointers(info.element, path, false, out); });
    case type_kind::matrix:
        return each(info.count * _p.at(info.element).count, [&](u32) { leaf(); });
    case type_kind::vector:
    {
        if (!is_column)
            return each(info.count, [&](u32) { leaf(); });
        auto const first = path.back();
        for (auto k = u32(0); k < info.count; ++k)
        {
            path.back() = index_sum(first, _m.constant_i32(k));
            leaf();
        }
        path.back() = first;
        return;
    }
    default:
        return leaf();
    }
}

cc::vector<ll_value> writer::payload_pointers_of(place const& p, type_ref t)
{
    auto path = cc::vector<ll_value>::create_copy_of(
        cc::span<ll_value const>(_paths.data() + p.path_first, isize(p.path_count)));
    auto out = cc::vector<ll_value>();
    _reached = &_payload_roots[p.root];
    payload_pointers(t, path, p.is_column, out);
    return out;
}

writer::place writer::place_of(value_ref pointer)
{
    if (is_local(pointer))
        return {.kind = place::kind_t::memory, .root = u32(_local_memory[index_of(pointer)])};
    if (is_global(pointer))
    {
        auto const index = index_of(pointer);
        auto const& g = _p.globals[index];
        auto const space = _p.at(g.type).space;
        if (space == storage::task_payload || space == storage::ray_input || space == storage::hit_attribute
            || space == storage::ray_output || space == storage::callable_output)
        {
            auto result = place{.kind = place::kind_t::payload, .root = index};
            if (_payload_roots[index].is_wrapped)
            {
                result.path_first = u32(_paths.size());
                result.path_count = 1;
                _paths.push_back(_m.constant_i32(0));
            }
            return result;
        }
        switch (g.kind)
        {
        case global_kind::constants:
        case global_kind::inline_constants:
            return {.kind = place::kind_t::constants, .root = index};
        case global_kind::buffer:
        case global_kind::bytes:
            return {.kind = place::kind_t::buffer, .root = index};
        default:
            if (_global_memory[index] >= 0)
                return {.kind = place::kind_t::memory, .root = u32(_global_memory[index])};
            return {};
        }
    }
    return _places[index_of(pointer)];
}

void writer::texel_pointer(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const image = value_ref(operands[0]);
    auto result = is_global(image) ? place{.root = u32(index_of(image))} : _places[index_of(image)];
    result.kind = place::kind_t::texel;
    auto const coordinate = value_ref(operands[1]);
    result.path_first = u32(_paths.size());
    result.path_count = leaves_of(_p.type_of(coordinate));
    for (auto l = u32(0); l < result.path_count; ++l)
        _paths.push_back(leaf_at(coordinate, l));
    _places[index] = result;
}

ll_value writer::add_dynamic(ll_value dynamic, ll_value index, u32 scale)
{
    auto const scaled = scale == 1 ? index : _m.binary(binary_op::mul, index, _m.constant_i32(scale));
    return dynamic == ll_value::none ? scaled : _m.binary(binary_op::add, dynamic, scaled);
}

void writer::access_chain(u32 index, instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const base = value_ref(operands[0]);
    auto result = place_of(base);
    if (result.kind == place::kind_t::payload)
        return payload_chain(index, i);
    auto type = _p.at(_p.type_of(base)).element;
    auto const is_memory = result.kind == place::kind_t::memory;
    auto added = cc::vector<memory_term>();

    auto first = isize(1);
    // a binding array's element is a resource of its own, which only here has a handle
    if (is_global(base) && _handles[index_of(base)].is_array)
    {
        result.handle = made_handle(_handles[index_of(base)], leaf_at(value_ref(operands[1])), i.aux == k_ir_nonuniform);
        type = _p.at(type).element;
        first = 2;
    }

    for (auto k = first; k < operands.size(); ++k)
    {
        auto const index_value = value_ref(operands[k]);
        auto const is_known = is_constant(index_value);
        auto const known = is_known ? _p.constants[index_of(index_value)].low : 0u;
        auto const& info = _p.at(type);
        if (info.kind == type_kind::structure)
        {
            auto const& m = _p.members[info.first_member + known];
            if (is_memory)
            {
                for (auto n = u32(0); n < known; ++n)
                    result.offset += leaves_of(_p.members[info.first_member + n].type);
            }
            else
            {
                result.offset += u32(m.offset);
                result.matrix_stride = m.matrix_stride;
            }
            type = m.type;
            continue;
        }
        if (info.kind == type_kind::runtime_array)
        {
            // a buffer's element, the one index a byte offset cannot say
            if (result.kind == place::kind_t::buffer && _p.globals[result.root].kind == global_kind::bytes)
                result.index_bytes = word_address(index_value);
            auto const value = leaf_at(index_value);
            result.index = value;
            result.stride = u32(info.stride);
            type = info.element;
            continue;
        }

        auto const element = info.element;
        auto step = u32(0);
        if (is_memory)
            step = leaves_of(element);
        else if (info.kind == type_kind::array)
            step = u32(info.stride);
        else if (info.kind == type_kind::matrix)
            step = u32(result.matrix_stride);
        else
            step = _p.at(element).width / 8;
        if (info.kind == type_kind::vector)
            result.matrix_stride = 0;
        if (is_known)
            result.offset += known * step;
        else if (is_memory)
            added.push_back({.index = leaf_at(index_value), .stride = step, .count = info.count});
        else
            result.dynamic = add_dynamic(result.dynamic, leaf_at(index_value), step);
        type = element;
    }
    if (!added.empty())
    {
        auto const first = u32(_terms.size());
        for (auto t = u32(0); t < result.term_count; ++t)
            _terms.push_back(_terms[isize(result.term_first + t)]);
        _terms.push_back_range(added);
        result.term_first = first;
        result.term_count = u32(_terms.size()) - first;
    }
    _places[index] = result;
}

ll_value writer::memory_pointer(place const& p, u32 leaf)
{
    auto const& held = _memories[p.root];
    auto const at = p.offset + leaf;
    if (at >= held.leaves)
    {
        unsupported("an index past the end of a variable");
        return held.columns[0];
    }
    auto const column = held.column_of[isize(at)];
    auto const position = held.position_of[isize(at)];
    // each index steps by what one element of its array holds of this column, which the next element's leaf shows
    auto dynamic = ll_value::none;
    for (auto t = u32(0); t < p.term_count; ++t)
    {
        auto const& term = _terms[isize(p.term_first + t)];
        if (term.count < 2)
            continue;
        auto const next = at + term.stride;
        if (next >= held.leaves || held.column_of[isize(next)] != column)
        {
            unsupported("an index known at run time alone into a level its variable splits");
            return held.columns[0];
        }
        dynamic = add_dynamic(dynamic, term.index, held.position_of[isize(next)] - position);
    }
    auto const storage = held.columns[column];
    auto const index = dynamic == ll_value::none ? _m.constant_i32(position)
                                                 : _m.binary(binary_op::add, dynamic, _m.constant_i32(position));
    ll_value const indices[] = {_m.constant_i32(0), index};
    return _m.element_pointer(_m.element_of(_m.type_of(storage)), storage, indices);
}

void writer::load_memory(place const& p, type_ref t, cc::vector<ll_value>& out)
{
    if (p.kind == place::kind_t::payload)
    {
        auto const pointers = payload_pointers_of(p, t);
        for (auto l = u32(0); l < leaves_of(t); ++l)
        {
            auto const wanted = scalar_type(scalar_at(t, l));
            auto const is_bool = wanted == _m.bool_type();
            auto const held = is_bool ? _m.int_type() : wanted;
            auto const loaded = _m.load(held, pointers[l], _m.bits_of(held) / 8);
            out.push_back(is_bool ? _m.compare(predicate::i_ne, loaded, _m.constant_i32(0)) : loaded);
        }
        return;
    }
    if (p.kind == place::kind_t::memory)
    {
        for (auto l = u32(0); l < leaves_of(t); ++l)
        {
            auto const scalar = scalar_at(t, l);
            auto const wanted = scalar_type(scalar);
            auto const is_bool = wanted == _m.bool_type();
            auto const held = is_bool ? _m.int_type() : wanted;
            auto const loaded = _m.load(held, memory_pointer(p, l), _m.bits_of(held) / 8);
            out.push_back(is_bool ? _m.compare(predicate::i_ne, loaded, _m.constant_i32(0)) : loaded);
        }
        return;
    }
    load_bytes(p, t, p.offset, p.matrix_stride, out);
}

void writer::store_memory(place const& p, type_ref t, cc::span<ll_value const> leaves)
{
    if (p.kind == place::kind_t::payload)
    {
        auto const pointers = payload_pointers_of(p, t);
        for (auto l = u32(0); l < leaves_of(t); ++l)
        {
            auto value = leaves[l];
            if (_m.type_of(value) == _m.bool_type())
                value = _m.cast(cast_op::zext, value, _m.int_type());
            _m.store(pointers[l], value, _m.bits_of(_m.type_of(value)) / 8);
        }
        return;
    }
    if (p.kind == place::kind_t::memory)
    {
        for (auto l = u32(0); l < leaves_of(t); ++l)
        {
            auto value = leaves[l];
            if (_m.type_of(value) == _m.bool_type())
                value = _m.cast(cast_op::zext, value, _m.int_type());
            _m.store(memory_pointer(p, l), value, _m.bits_of(_m.type_of(value)) / 8);
        }
        return;
    }
    auto at = u32(0);
    store_bytes(p, t, p.offset, p.matrix_stride, leaves, at);
}

ll_value writer::word_address(value_ref index)
{
    // `bytes >> 2` multiplied back by 4 is `bytes & ~3`, and an offset no multiple of 4 is a program error (EVAL-97),
    // so the address is the offset as HLSL's `Load` takes it; the words after it, `(bytes >> 2) + k`, are 4 k past it
    auto words = u32(0);
    auto at = index;
    while (is_operation(at))
    {
        auto const& i = _p.instructions[index_of(at)];
        auto const operands = _p.operands_of(i);
        if (i.code != op::iadd || operands.size() != 2)
            break;
        auto const left = value_ref(operands[0]);
        auto const right = value_ref(operands[1]);
        if (is_constant(right))
        {
            words += _p.constants[index_of(right)].low;
            at = left;
        }
        else if (is_constant(left))
        {
            words += _p.constants[index_of(left)].low;
            at = right;
        }
        else
            break;
    }
    if (is_operation(at))
    {
        auto const& i = _p.instructions[index_of(at)];
        auto const operands = _p.operands_of(i);
        auto const by = value_ref(operands.size() == 2 ? operands[1] : 0u);
        if (i.code == op::shift_right_logical && is_constant(by) && _p.constants[index_of(by)].low == 2)
        {
            auto added = 4 * words;
            auto const base = linear_of(value_ref(operands[0]), added);
            if (base == ll_value::none)
                return _m.constant_i32(added);
            return added == 0 ? base : _m.binary(binary_op::add, base, _m.constant_i32(added));
        }
    }
    return _m.binary(binary_op::mul, leaf_at(index), _m.constant_i32(4));
}

ll_value writer::linear_of(value_ref v, u32& added, int depth)
{
    if (is_constant(v))
    {
        added += _p.constants[index_of(v)].low;
        return ll_value::none;
    }
    if (!is_operation(v) || depth > 8)
        return leaf_at(v);
    auto const& i = _p.instructions[index_of(v)];
    auto const operands = _p.operands_of(i);
    if (i.code == op::iadd && operands.size() == 2)
    {
        auto inner = u32(0);
        auto const a = linear_of(value_ref(operands[0]), inner, depth + 1);
        auto const b = linear_of(value_ref(operands[1]), inner, depth + 1);
        added += inner;
        if (a == ll_value::none)
            return b;
        if (b == ll_value::none)
            return a;
        return _m.binary(binary_op::add, a, b);
    }
    if (i.code == op::imul && operands.size() == 2)
    {
        auto const left = value_ref(operands[0]);
        auto const right = value_ref(operands[1]);
        auto const by = is_constant(left) ? left : is_constant(right) ? right : value_ref::none;
        if (is_valid(by))
        {
            auto const factor = _p.constants[index_of(by)].low;
            auto inner = u32(0);
            auto const base = linear_of(by == left ? right : left, inner, depth + 1);
            added += inner * factor;
            return base == ll_value::none ? ll_value::none : _m.binary(binary_op::mul, base, _m.constant_i32(factor));
        }
    }
    return leaf_at(v);
}

ll_value writer::raw_address(place const& p, u32 offset)
{
    auto const word
        = p.index_bytes != ll_value::none ? p.index_bytes : _m.binary(binary_op::mul, p.index, _m.constant_i32(4));
    if (p.dynamic == ll_value::none && offset == 0)
        return word;
    return _m.binary(binary_op::add, word, byte_offset(p, offset));
}

ll_value writer::byte_offset(place const& p, u32 offset)
{
    return p.dynamic == ll_value::none ? _m.constant_i32(offset)
                                       : _m.binary(binary_op::add, p.dynamic, _m.constant_i32(offset));
}

ll_value writer::constant_lane(place const& p, type_ref scalar, u32 offset)
{
    auto const type = scalar_type(scalar);
    auto const size = _m.bits_of(type) / 8;
    auto const handle = handle_at(p);
    auto row = ll_value::none;
    auto lane = (offset & 15) / size;
    if (p.dynamic == ll_value::none)
        row = _m.constant_i32(offset >> 4);
    else
    {
        // an index past a row's reach steps by whole rows, which a block's array always does
        auto const shifted = _m.binary(binary_op::lshr, p.dynamic, _m.constant_i32(4));
        row = _m.binary(binary_op::add, shifted, _m.constant_i32(offset >> 4));
    }
    for (auto const& known : _rows)
        if (known.handle == handle && known.row == row && known.overload == type)
            return _m.extract_value(known.fetched, lane);
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(dx_op::cbuffer_load_legacy)].number), handle, row};
    auto const fetched = call(dx_op::cbuffer_load_legacy, type, args, true);
    _rows.push_back({.handle = handle, .row = row, .overload = type, .fetched = fetched});
    return _m.extract_value(fetched, lane);
}

void writer::buffer_lanes(place const& p, type_ref scalar, u32 offset, u32 lanes, cc::vector<ll_value>& out)
{
    auto const type = scalar_type(scalar);
    auto const size = _m.bits_of(type) / 8;
    auto const handle = handle_at(p);
    auto const is_raw = _p.globals[p.root].kind == global_kind::bytes;
    auto const element = is_raw ? p.index : p.index;
    auto const number = _m.constant_i32(k_dx_ops[isize(dx_op::raw_buffer_load)].number);
    auto done = u32(0);
    while (done < lanes)
    {
        auto const now = cc::min(lanes - done, u32(4));
        auto const at = offset + done * size;
        auto first = ll_value::none;
        auto second = ll_value::none;
        if (is_raw)
        {
            // a byte address alone, the element being a word
            first = element == ll_value::none ? byte_offset(p, at) : raw_address(p, at);
            second = _m.constant_undef(_m.int_type());
        }
        else
        {
            first = element;
            second = byte_offset(p, at);
        }
        ll_value const args[] = {number, handle, first, second, _m.constant_i8((1u << now) - 1), _m.constant_i32(size)};
        // a buffer the shader cannot write holds what it held, whichever load asks
        auto const fetched = call(dx_op::raw_buffer_load, type, args, _p.globals[p.root].is_read_only);
        for (auto l = u32(0); l < now; ++l)
            out.push_back(_m.extract_value(fetched, l));
        done += now;
    }
}

void writer::store_lanes(place const& p, type_ref scalar, u32 offset, cc::span<ll_value const> lanes)
{
    auto const type = scalar_type(scalar);
    auto const size = _m.bits_of(type) / 8;
    auto const handle = handle_at(p);
    auto const is_raw = _p.globals[p.root].kind == global_kind::bytes;
    auto const number = _m.constant_i32(k_dx_ops[isize(dx_op::raw_buffer_store)].number);
    auto const undefined = _m.constant_undef(type);
    auto done = isize(0);
    while (done < lanes.size())
    {
        auto const now = cc::min(lanes.size() - done, isize(4));
        auto const at = offset + u32(done) * size;
        auto first = ll_value::none;
        auto second = ll_value::none;
        if (is_raw)
        {
            first = raw_address(p, at);
            second = _m.constant_undef(_m.int_type());
        }
        else
        {
            first = p.index;
            second = byte_offset(p, at);
        }
        ll_value const args[] = {number,
                                 handle,
                                 first,
                                 second,
                                 lanes[done],
                                 now > 1 ? lanes[done + 1] : undefined,
                                 now > 2 ? lanes[done + 2] : undefined,
                                 now > 3 ? lanes[done + 3] : undefined,
                                 _m.constant_i8((1u << now) - 1),
                                 _m.constant_i32(size)};
        (void)call(dx_op::raw_buffer_store, type, args);
        done += now;
    }
}

void writer::load_bytes(place const& p, type_ref t, u32 offset, i32 matrix_stride, cc::vector<ll_value>& out)
{
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::bool_:
    case type_kind::int_:
    case type_kind::float_:
        if (p.kind == place::kind_t::constants)
            out.push_back(constant_lane(p, t, offset));
        else
            buffer_lanes(p, t, offset, 1, out);
        return;
    case type_kind::vector:
        if (p.kind == place::kind_t::buffer)
            return buffer_lanes(p, info.element, offset, info.count, out);
        for (auto l = u32(0); l < info.count; ++l)
            out.push_back(constant_lane(p, info.element, offset + l * (_p.at(info.element).width / 8)));
        return;
    case type_kind::matrix:
        for (auto c = u32(0); c < info.count; ++c)
            load_bytes(p, info.element, offset + c * u32(matrix_stride), 0, out);
        return;
    case type_kind::array:
        for (auto e = u32(0); e < info.count; ++e)
            load_bytes(p, info.element, offset + e * u32(info.stride), matrix_stride, out);
        return;
    case type_kind::structure:
        for (auto m = u32(0); m < info.member_count; ++m)
        {
            auto const& held = _p.members[info.first_member + m];
            load_bytes(p, held.type, offset + u32(held.offset), held.matrix_stride, out);
        }
        return;
    default:
        return unsupported("a load of this type from GPU memory");
    }
}

void writer::store_bytes(place const& p, type_ref t, u32 offset, i32 matrix_stride, cc::span<ll_value const> leaves, u32& at)
{
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::bool_:
    case type_kind::int_:
    case type_kind::float_:
        store_lanes(p, t, offset, leaves.subspan({.offset = isize(at), .size = 1}));
        ++at;
        return;
    case type_kind::vector:
        store_lanes(p, info.element, offset, leaves.subspan({.offset = isize(at), .size = isize(info.count)}));
        at += info.count;
        return;
    case type_kind::matrix:
        for (auto c = u32(0); c < info.count; ++c)
            store_bytes(p, info.element, offset + c * u32(matrix_stride), 0, leaves, at);
        return;
    case type_kind::array:
        for (auto e = u32(0); e < info.count; ++e)
            store_bytes(p, info.element, offset + e * u32(info.stride), matrix_stride, leaves, at);
        return;
    case type_kind::structure:
        for (auto m = u32(0); m < info.member_count; ++m)
        {
            auto const& held = _p.members[info.first_member + m];
            store_bytes(p, held.type, offset + u32(held.offset), held.matrix_stride, leaves, at);
        }
        return;
    default:
        return unsupported("a store of this type to GPU memory");
    }
}

void writer::load(u32 index, instruction const& i)
{
    auto const pointer = value_ref(_p.operands_of(i)[0]);
    if (is_global(pointer))
    {
        auto const& g = _p.globals[index_of(pointer)];
        auto const& target = _p.at(_p.at(g.type).element);
        if (target.kind == type_kind::image || target.kind == type_kind::sampler
            || target.kind == type_kind::acceleration_structure)
            return define_one(index, handle_of(pointer));
    }
    auto const where = place_of(pointer);
    if (where.handle != ll_value::none && where.kind == place::kind_t::none)
        return define_one(index, where.handle);
    if (where.kind == place::kind_t::none)
        return unsupported("a load of something that is no memory");
    _loaded.clear();
    load_memory(where, i.type, _loaded);
    _scratch.clear();
    for (auto const v : _loaded)
        _scratch.push_back(v);
    define(index);
}

void writer::store(instruction const& i)
{
    auto const operands = _p.operands_of(i);
    auto const where = place_of(value_ref(operands[0]));
    if (where.kind == place::kind_t::none)
        return unsupported("a store to something that is no memory");
    auto const value = value_ref(operands[1]);
    auto const held = _p.at(_p.type_of(value_ref(operands[0]))).element;
    _loaded.clear();
    for (auto l = u32(0); l < count_of(value); ++l)
        _loaded.push_back(leaf_at(value, l));
    store_memory(where, held, _loaded);
}
