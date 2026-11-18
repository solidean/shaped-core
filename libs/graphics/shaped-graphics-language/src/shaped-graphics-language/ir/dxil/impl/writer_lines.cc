#include "writer.hh"

#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil::impl;

// EMIT-257 and EMIT-267: the debug info of DXC's -Zi.
// One `DISubprogram` scopes the entry point's own lines, and one each function inlined into it, named as written.
// At `full` each variable is a `DILocalVariable` of its function, and each leaf of a value a `llvm.dbg.value` of it,
// a `DW_OP_bit_piece` of the packed value where the variable has more than one leaf, as DXC states HLSL's.

namespace
{
// LLVM 3.7's metadata records
constexpr u32 k_di_location = 7;
constexpr u32 k_di_subrange = 13;
constexpr u32 k_di_basic_type = 15;
constexpr u32 k_di_file = 16;
constexpr u32 k_di_derived_type = 17;
constexpr u32 k_di_composite_type = 18;
constexpr u32 k_di_subroutine_type = 19;
constexpr u32 k_di_compile_unit = 20;
constexpr u32 k_di_subprogram = 21;
constexpr u32 k_di_lexical_block = 22;
constexpr u32 k_di_local_variable = 28;
constexpr u32 k_di_expression = 29;

constexpr u64 k_dw_tag_array_type = 0x01;
constexpr u64 k_dw_tag_class_type = 0x02;
constexpr u64 k_dw_tag_member = 0x0d;
constexpr u64 k_dw_tag_structure_type = 0x13;
constexpr u64 k_dw_tag_base_type = 0x24;
constexpr u64 k_dw_tag_auto_variable = 0x100;
constexpr u64 k_dw_tag_arg_variable = 0x101;
constexpr u64 k_dw_ate_boolean = 0x02;
constexpr u64 k_dw_ate_float = 0x04;
constexpr u64 k_dw_ate_signed = 0x05;
constexpr u64 k_dw_ate_unsigned = 0x08;
constexpr u64 k_dw_op_bit_piece = 0x9d;

constexpr u64 k_dw_lang_c_plus_plus = 4;
constexpr u64 k_full_debug = 1;
} // namespace

void writer::locate(instruction const& i)
{
    if (_p.lines.is_empty())
        return;
    auto const place = _p.lines.place_of(i.statement);
    auto const is_named = place.file >= 0 && place.file < i32(_p.lines.names.size());
    _m.set_location(is_named ? i.statement + 1 : 0);
}

void writer::write_debug_info()
{
    if (_p.lines.is_empty())
        return;
    _m.set_location(0);
    auto const& lines = _p.lines;
    auto const count = u32(lines.statements.size());
    auto const used = _m.locations_used(count);
    auto const file_count = isize(lines.names.size());

    // what the located operations name: each file, and the file of the entry point's own body
    auto names_file = cc::vector<bool>::create_filled(file_count, false);
    auto entry_file = i32(-1);
    for (auto s = u32(0); s < count; ++s)
    {
        if (!used[s])
            continue;
        auto const& place = lines.statements[s];
        names_file[place.file] = true;
        for (auto k = u32(0); k < place.calls_count; ++k)
            if (auto const f = lines.calls[place.calls_first + k].file; f >= 0 && f < file_count)
                names_file[f] = true;
        if (entry_file < 0)
            entry_file
                = place.calls_count == 0 ? place.file : lines.calls[place.calls_first + place.calls_count - 1].file;
    }
    if (entry_file < 0)
        return;
    // a variable names the file of the statement that declares it
    for (auto const& key : _debug_keys)
        if (auto const f = lines.place_of(_p.debug_variables[key.variable].statement).file; f >= 0 && f < file_count)
            names_file[f] = true;

    _m.begin_debug_metadata();
    auto files = cc::vector<u32>::create_filled(file_count, 0u);
    for (auto f = isize(0); f < file_count; ++f)
        if (names_file[f])
        {
            u64 const fields[] = {0, _m.md_string(lines.names[f]), 0};
            files[f] = _m.md_record(k_di_file, fields);
        }
    u32 const no_types[] = {0};
    u64 const subroutine_fields[] = {0, 0, _m.md_node(no_types)};
    auto const subroutine = _m.md_record(k_di_subroutine_type, subroutine_fields);

    auto const subprogram = [&](u32 name, i32 file, u32 function)
    {
        u64 const fields[]
            = {0, files[file], name, 0, files[file], 1, subroutine, 0, 1, 1, 0, 0, 0, 0, 0, function, 0, 0, 0};
        return _m.md_record(k_di_subprogram, fields);
    };
    auto subprograms = cc::vector<u32>();
    auto const entry = subprogram(_m.md_string(_p.entry_name), entry_file, _m.md_value(_m.type_of(_body), _body));
    subprograms.push_back(entry);
    // what an inlined function's lines stand in, which names no function of the module; by file where the line table
    // knows no function
    auto inlined = cc::vector<u32>::create_filled(lines.functions.size(), 0u);
    auto inlined_file = cc::vector<u32>::create_filled(file_count, 0u);
    auto const inlined_scope = [&](i32 f, i32 function)
    {
        // a statement no line names, as one of the prelude's, stands in the entry point's file
        if (f < 0 || f >= file_count || files[f] == 0)
            f = entry_file;
        auto& scope = function >= 0 && function < i32(inlined.size()) ? inlined[function] : inlined_file[f];
        if (scope == 0)
        {
            auto const name = function >= 0 && function < i32(inlined.size())
                                ? cc::string_view(lines.functions[function])
                                : cc::string_view("<inlined>");
            scope = subprogram(_m.md_string(name), f, 0);
            subprograms.push_back(scope);
        }
        return scope;
    };
    auto const function_scope_of = [&](source_place const& place)
    { return place.calls_count == 0 && place.file == entry_file ? entry : inlined_scope(place.file, place.function); };
    // EMIT-267: a block of the line table in the function scope its place stands in, made with its parents on first use
    auto blocks = cc::vector<u32>::create_filled(lines.blocks.size(), 0u);
    auto const block_scope = [&](auto const& self, i32 b, u32 function) -> u32
    {
        if (blocks[b] != 0)
            return blocks[b];
        auto const& block = lines.blocks[b];
        auto const parent = block.parent >= 0 ? self(self, block.parent, function) : function;
        auto const file = block.file >= 0 && block.file < file_count && files[block.file] != 0 ? files[block.file]
                                                                                               : files[entry_file];
        u64 const fields[] = {0, parent, file, u64(block.line), 0};
        blocks[b] = _m.md_record(k_di_lexical_block, fields);
        return blocks[b];
    };
    auto const scope_of = [&](source_place const& place)
    {
        auto const function = function_scope_of(place);
        return place.block >= 0 && place.block < blocks.size() ? block_scope(block_scope, place.block, function)
                                                               : function;
    };

    // each location's scope, and the chain of calls it was inlined through, outermost first
    auto locations = cc::vector<module_builder::debug_location>::create_defaulted(count);
    for (auto s = u32(0); s < count; ++s)
    {
        if (!used[s])
            continue;
        auto const& place = lines.statements[s];
        auto at = u32(0);
        for (auto k = place.calls_count; k > 0; --k)
        {
            auto const& call = lines.calls[place.calls_first + k - 1];
            auto const scope = k == place.calls_count ? entry : inlined_scope(call.file, call.function);
            // a location record names its scope by its position, unlike every other operand here
            u64 const fields[] = {1, u64(call.line), 0, u64(scope - 1), at};
            at = _m.md_record(k_di_location, fields);
        }
        locations[s] = {.line = u32(place.line), .column = 0, .scope = scope_of(place), .inlined_at = at};
    }
    _m.set_debug_locations(cc::move(locations));

    // EMIT-267: each variable a value is stated of, in the scope of the function that declares it
    if (!_debug_keys.empty())
    {
        auto variables = cc::vector<u32>::create_filled(_p.debug_variables.size(), 0u);
        auto parameters = u64(0);
        u64 const whole_fields[] = {0};
        auto const whole = _m.md_record(k_di_expression, whole_fields);
        auto values = cc::vector<module_builder::debug_value_metadata>();
        auto offsets = cc::vector<u32>();
        auto sizes = cc::vector<u32>();
        for (auto const& key : _debug_keys)
        {
            auto const& v = _p.debug_variables[key.variable];
            if (variables[key.variable] == 0)
            {
                auto const place = lines.place_of(v.statement);
                auto const file = place.file >= 0 && place.file < file_count ? files[place.file] : files[entry_file];
                u64 const fields[] = {0,
                                      v.is_parameter ? k_dw_tag_arg_variable : k_dw_tag_auto_variable,
                                      scope_of(place),
                                      _m.md_string(_p.name_at(v.name_first, v.name_size)),
                                      file,
                                      u64(place.line),
                                      debug_type(v.type),
                                      v.is_parameter ? ++parameters : 0,
                                      0};
                variables[key.variable] = _m.md_record(k_di_local_variable, fields);
            }
            offsets.clear();
            sizes.clear();
            auto at = u32(0);
            debug_leaves(v.type, offsets, sizes, at);
            auto expression = whole;
            if (!key.is_declare && offsets.size() > 1 && key.leaf < offsets.size())
            {
                u64 const fields[] = {0, k_dw_op_bit_piece, offsets[key.leaf], sizes[key.leaf]};
                expression = _m.md_record(k_di_expression, fields);
            }
            values.push_back({.variable = variables[key.variable], .expression = expression});
        }
        for (auto k = isize(0); k < _debug_keys.size(); ++k)
            if (is_valid(_debug_keys[k].constant))
                _m.set_debug_operand(u32(k), leaf_at(_debug_keys[k].constant, _debug_keys[k].constant_leaf));
        _m.set_debug_values(cc::move(values));
    }

    auto const empty = _m.md_node(cc::span<u32 const>());
    u64 const unit_fields[] = {1,
                               k_dw_lang_c_plus_plus,
                               files[entry_file],
                               _m.md_string("sgl"),
                               0,
                               0,
                               0,
                               0,
                               k_full_debug,
                               empty,
                               0,
                               _m.md_node(subprograms),
                               0,
                               0,
                               0};
    auto const unit = _m.md_record(k_di_compile_unit, unit_fields);
    u32 const units[] = {unit};
    _m.md_named("llvm.dbg.cu", units);

    u32 const dwarf[] = {_m.md_i32(2), _m.md_string("Dwarf Version"), _m.md_i32(4)};
    u32 const debug_version[] = {_m.md_i32(2), _m.md_string("Debug Info Version"), _m.md_i32(3)};
    u32 const flags[] = {_m.md_node(dwarf), _m.md_node(debug_version)};
    _m.md_named("llvm.module.flags", flags);

    // the sources themselves, so a debugger shows each line without the files
    auto contents = cc::vector<u32>();
    for (auto f = isize(0); f < file_count; ++f)
        if (names_file[f])
        {
            u32 const pair[] = {_m.md_string(lines.names[f]), _m.md_string(lines.texts[f])};
            contents.push_back(_m.md_node(pair));
        }
    _m.md_named("dx.source.contents", contents);
    u32 const nothing[] = {empty};
    _m.md_named("dx.source.defines", nothing);
    u32 const main_name[] = {_m.md_string(lines.names[entry_file])};
    u32 const main_file[] = {_m.md_node(main_name)};
    _m.md_named("dx.source.mainFileName", main_file);
    _m.md_named("dx.source.args", nothing);
}

void writer::declare(instruction const& i)
{
    auto const local = value_ref(_p.operands_of(i)[1]);
    if (!is_local(local) || _local_memory[index_of(local)] < 0)
        return;
    // a local is one array per kind of scalar, so only one holding a single kind lays its leaves out as the variable
    // does; one of several kinds is not declared yet
    auto const& memory = _memories[isize(_local_memory[index_of(local)])];
    auto pointer = ll_value::none;
    for (auto const held : memory.storage)
    {
        if (held == ll_value::none)
            continue;
        if (pointer != ll_value::none)
            return;
        pointer = held;
    }
    if (pointer == ll_value::none)
        return;
    locate(i);
    _m.debug_declare(pointer, u32(_debug_keys.size()));
    _debug_keys.push_back({.variable = _p.operands_of(i)[0], .is_declare = true});
}

void writer::describe(instruction const& i)
{
    if (!_p.describes_variables || !is_debug(i.code) || _p.lines.is_empty())
        return;
    auto const operands = _p.operands_of(i);
    if (i.code == op::debug_declare)
        return declare(i);
    auto const value = value_ref(operands[1]);
    // a value a pass removed is optimized out from here, which no dbg.value is stated for yet
    if (!is_valid(value))
        return;
    locate(i);
    auto const count = leaves_of(_p.type_of(value));
    for (auto k = u32(0); k < count; ++k)
    {
        // a constant's leaves made here would be constants of the program, so they are made with the debug info's
        auto const is_constant_value = is_constant(value);
        _m.debug_value(is_constant_value ? ll_value::none : leaf_at(value, k), u32(_debug_keys.size()));
        _debug_keys.push_back({.variable = operands[0],
                               .leaf = operands[2] + k,
                               .constant = is_constant_value ? value : value_ref::none,
                               .constant_leaf = k});
    }
}

void writer::debug_leaves(type_ref t, cc::vector<u32>& offsets, cc::vector<u32>& sizes, u32& offset)
{
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::vector:
    case type_kind::matrix:
    case type_kind::array:
        for (auto k = u32(0); k < info.count; ++k)
            debug_leaves(info.element, offsets, sizes, offset);
        return;
    case type_kind::structure:
        for (auto m = info.first_member; m < info.first_member + info.member_count; ++m)
            debug_leaves(_p.members[m].type, offsets, sizes, offset);
        return;
    default:
    {
        // a bool takes a word, as HLSL stores one
        auto const bits = info.kind == type_kind::bool_ || info.width == 0 ? 32u : u32(info.width);
        offsets.push_back(offset);
        sizes.push_back(bits);
        offset += bits;
        return;
    }
    }
}

u32 writer::debug_type(type_ref t)
{
    if (!is_valid(t))
        return 0;
    if (_debug_types.size() < _p.types.size())
        _debug_types.resize_to_filled(_p.types.size(), 0u);
    if (auto const known = _debug_types[isize(t)]; known != 0)
        return known;

    auto const& info = _p.at(t);
    auto offsets = cc::vector<u32>();
    auto sizes = cc::vector<u32>();
    auto bits = u32(0);
    debug_leaves(t, offsets, sizes, bits);
    auto const member = [&](cc::string_view name, u32 base, u64 size, u64 offset)
    {
        u64 const fields[] = {0, k_dw_tag_member, _m.md_string(name), 0, 0, 0, base, size, 32, offset, 0, 0};
        return _m.md_record(k_di_derived_type, fields);
    };
    auto const composite = [&](u64 tag, cc::string_view name, u32 base, cc::span<u32 const> elements)
    {
        u64 const fields[]
            = {0, tag, _m.md_string(name), 0, 0, 0, base, bits, 32, 0, 0, _m.md_node(elements), 0, 0, 0, 0};
        return _m.md_record(k_di_composite_type, fields);
    };
    auto const scalar_name = [&](sgl::ir::type const& scalar) -> cc::string
    {
        if (scalar.kind == type_kind::bool_)
            return "bool";
        if (scalar.kind == type_kind::float_)
            return scalar.width == 16 ? "half" : scalar.width == 64 ? "double" : "float";
        auto name = cc::string(scalar.is_signed ? "int" : "uint");
        if (scalar.width != 32)
            name.appendf("{}_t", scalar.width);
        return name;
    };

    auto result = u32(0);
    switch (info.kind)
    {
    case type_kind::bool_:
    case type_kind::int_:
    case type_kind::float_:
    {
        auto const encoding = info.kind == type_kind::bool_  ? k_dw_ate_boolean
                            : info.kind == type_kind::float_ ? k_dw_ate_float
                            : info.is_signed                 ? k_dw_ate_signed
                                                             : k_dw_ate_unsigned;
        u64 const fields[] = {0, k_dw_tag_base_type, _m.md_string(scalar_name(info)), bits, 32, encoding};
        result = _m.md_record(k_di_basic_type, fields);
        break;
    }
    case type_kind::vector:
    {
        auto const element = debug_type(info.element);
        constexpr cc::string_view lanes[] = {"x", "y", "z", "w"};
        auto members = cc::vector<u32>();
        for (auto k = u32(0); k < info.count; ++k)
            members.push_back(member(k < 4 ? lanes[k] : cc::string_view("_"), element, sizes[k], offsets[k]));
        result = composite(k_dw_tag_class_type,
                           cc::format("vector<{}, {}>", scalar_name(_p.at(info.element)), info.count), 0, members);
        break;
    }
    case type_kind::matrix:
    {
        // columns of a column vector each, as SGL holds a matrix; `_rc` names row r of column c, from 1
        auto const& column = _p.at(info.element);
        auto const scalar = debug_type(column.element);
        auto members = cc::vector<u32>();
        for (auto c = u32(0); c < info.count; ++c)
            for (auto r = u32(0); r < column.count; ++r)
            {
                auto const leaf = c * column.count + r;
                members.push_back(member(cc::format("_{}{}", r + 1, c + 1), scalar, sizes[leaf], offsets[leaf]));
            }
        result = composite(
            k_dw_tag_class_type,
            cc::format("matrix<{}, {}, {}>", scalar_name(_p.at(column.element)), column.count, info.count), 0, members);
        break;
    }
    case type_kind::array:
    {
        u64 const range_fields[] = {0, info.count, 0};
        u32 const ranges[] = {_m.md_record(k_di_subrange, range_fields)};
        result = composite(k_dw_tag_array_type, "", debug_type(info.element), ranges);
        break;
    }
    case type_kind::structure:
    {
        auto members = cc::vector<u32>();
        auto at = u32(0);
        for (auto m = info.first_member; m < info.first_member + info.member_count; ++m)
        {
            auto const& held = _p.members[m];
            auto inner_offsets = cc::vector<u32>();
            auto inner_sizes = cc::vector<u32>();
            auto inner = u32(0);
            debug_leaves(held.type, inner_offsets, inner_sizes, inner);
            members.push_back(member(_p.name_at(held.name_first, held.name_size), debug_type(held.type), inner, at));
            at += inner;
        }
        result = composite(k_dw_tag_structure_type, _p.name_at(info.name_first, info.name_size), 0, members);
        break;
    }
    default:
        return 0;
    }
    _debug_types[isize(t)] = result;
    return result;
}
