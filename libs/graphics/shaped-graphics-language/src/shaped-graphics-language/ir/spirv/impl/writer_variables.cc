#include "writer.hh"

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::spirv::impl;

// EMIT-267: the debug info of DXC's -fspv-debug=vulkan-with-source, `NonSemantic.Shader.DebugInfo.100`.
// One `DebugFunction` scopes the entry point's own lines, and one each function inlined into it, named as written.
// Each variable is a `DebugLocalVariable` of its function, and each value of it a `DebugValue`, indexed down to the part
// it gives where that is not the whole.
// Every id the debug info takes comes after every other, so the module's own instructions are those of `none`.
// An id is taken before the module's others are all known, so a debug instruction names a handle until the end.

namespace
{
// NonSemantic.Shader.DebugInfo.100's instructions
namespace debug
{
enum : u32
{
    compilation_unit = 1,
    type_basic = 2,
    type_array = 5,
    type_vector = 6,
    type_function = 8,
    type_composite = 10,
    type_member = 11,
    function = 20,
    scope = 23,
    inlined_at = 25,
    local_variable = 26,
    declare = 28,
    value = 29,
    expression = 31,
    source = 35,
    function_definition = 101,
    source_continued = 102,
    line = 103,
    type_matrix = 108,
};
}

constexpr u32 k_encoding_boolean = 2;
constexpr u32 k_encoding_float = 3;
constexpr u32 k_encoding_signed = 4;
constexpr u32 k_encoding_unsigned = 6;
constexpr u32 k_flag_public = 3;
constexpr u32 k_flag_local = 4;
constexpr u32 k_composite_structure = 1;
constexpr u32 k_language_unknown = 0;
constexpr u32 k_debug_info_version = 1;
constexpr u32 k_dwarf_version = 4;

/// The most text one `OpString` holds, well under an instruction's 65535 words.
constexpr isize k_source_chunk = 60000 * 4;

[[nodiscard]] cc::string_view scalar_name(ir::type const& scalar)
{
    if (scalar.kind == type_kind::bool_)
        return "bool";
    if (scalar.kind == type_kind::float_)
        return scalar.width == 16 ? "half" : scalar.width == 64 ? "double" : "float";
    switch (scalar.width)
    {
    case 8:
        return scalar.is_signed ? "int8_t" : "uint8_t";
    case 16:
        return scalar.is_signed ? "int16_t" : "uint16_t";
    case 64:
        return scalar.is_signed ? "int64_t" : "uint64_t";
    default:
        return scalar.is_signed ? "int" : "uint";
    }
}

/// Appends `from` to `into`, and where a handle stands in it to `patches`.
void append_patched(cc::vector<u32>& into,
                    cc::vector<isize>& patches,
                    cc::span<u32 const> from,
                    cc::span<isize const> from_patches)
{
    auto const offset = into.size();
    into.push_back_range(from);
    for (auto const at : from_patches)
        patches.push_back(offset + at);
}
} // namespace

u32 writer::debug_literal(u32 value)
{
    for (auto const& known : _debug_literals)
        if (known.value == value)
            return known.id;
    auto const handle = debug_handle();
    _debug_literals.push_back({.value = value, .id = handle});
    return handle;
}

void writer::debug_instruction(cc::vector<u32>& out,
                               cc::vector<isize>& patches,
                               u32 instruction,
                               std::initializer_list<u32> operands)
{
    debug_instruction(out, patches, debug_handle(), instruction, cc::span<u32 const>(operands.begin(), operands.size()));
}

void writer::debug_instruction(cc::vector<u32>& out,
                               cc::vector<isize>& patches,
                               u32 result,
                               u32 instruction,
                               cc::span<u32 const> operands)
{
    auto const word = [&](u32 w)
    {
        if ((w & k_debug_handle) != 0)
            patches.push_back(out.size());
        out.push_back(w);
    };
    head(out, spv::ExtInst, 5 + operands.size());
    out.push_back(id_of(_void));
    word(result);
    word(_debug_set);
    out.push_back(instruction);
    for (auto const o : operands)
        word(o);
}

u32 writer::debug_string(cc::string_view text)
{
    for (auto k = isize(0); k < _debug_string_texts.size(); ++k)
        if (_debug_string_texts[k] == text)
            return _debug_string_handles[k];
    _debug_string_texts.push_back(text);
    _debug_string_handles.push_back(debug_handle());
    return _debug_string_handles.back();
}

u32 writer::debug_scope(i32 file, i32 function)
{
    for (auto& s : _debug_scopes)
        if (s.function == function && (function != -2 || s.file == file))
        {
            if (s.file < 0)
                s.file = file;
            return s.handle;
        }
    _debug_scopes.push_back({.file = file, .function = function, .handle = debug_handle()});
    return _debug_scopes.back().handle;
}

u32 writer::debug_scope_of(source_place const& place)
{
    // a place inlined through no call stands in the entry point's own body
    if (place.calls_count == 0)
        return debug_scope(place.file, -1);
    return debug_scope(place.file, place.function >= 0 ? place.function : -2);
}

u32 writer::debug_inlined_at(source_place const& place)
{
    auto at = u32(0);
    for (auto k = place.calls_count; k > 0; --k)
    {
        auto const& call = _p.lines.calls[place.calls_first + k - 1];
        // the outermost call stands in the entry point, each other in the function the call before inlined
        auto const scope = k == place.calls_count ? debug_scope(call.file, -1)
                                                  : debug_scope(call.file, call.function >= 0 ? call.function : -2);
        auto found = u32(0);
        for (auto const& known : _debug_inlined)
            if (known.line == call.line && known.scope == scope && known.parent == at)
                found = known.handle;
        if (found == 0)
        {
            found = debug_handle();
            _debug_inlined.push_back({.line = call.line, .scope = scope, .parent = at, .handle = found});
        }
        at = found;
    }
    return at;
}

void writer::debug_line_before(u32 index)
{
    auto const place = _p.lines.place_of(_p.instructions[index].statement);
    if (place.file < 0 || place.file >= i32(_p.lines.names.size()))
        return;
    auto const scope = debug_scope_of(place);
    for (auto& s : _debug_scopes)
        if (s.handle == scope && (s.line == 0 || place.line < s.line))
            s.line = place.line;
    auto const inlined = debug_inlined_at(place);
    if (scope != _debug_scope_stated || inlined != _debug_inlined_stated)
    {
        if (inlined != 0)
            debug_instruction(_code, _debug_code_patches, debug::scope, {scope, inlined});
        else
            debug_instruction(_code, _debug_code_patches, debug::scope, {scope});
        _debug_scope_stated = scope;
        _debug_inlined_stated = inlined;
        _line_file = -1;
    }
    if (place.file == _line_file && place.line == _line)
        return;
    _line_file = place.file;
    _line = place.line;
    if (_debug_sources.empty())
        _debug_sources.resize_to_filled(_p.lines.names.size(), 0u);
    if (_debug_sources[place.file] == 0)
        _debug_sources[place.file] = debug_handle();
    auto const line = debug_literal(u32(place.line));
    auto const column = debug_literal(1);
    debug_instruction(_code, _debug_code_patches, debug::line, {_debug_sources[place.file], line, line, column, column});
}

void writer::debug_function_head()
{
    auto const entry = debug_scope(-1, -1);
    debug_instruction(_code, _debug_code_patches, debug::scope, {entry});
    debug_instruction(_code, _debug_code_patches, debug::function_definition, {entry, _body});
    _debug_scope_stated = entry;
    _debug_inlined_stated = 0;
}

void writer::describe(u32 index)
{
    auto const& i = _p.instructions[index];
    auto const operands = _p.operands_of(i);
    auto const variable = operands[0];
    if (variable >= u32(_p.debug_variables.size()))
        return;
    auto const& v = _p.debug_variables[variable];
    if (debug_type(v.type) == 0)
        return;
    if (_debug_variables.empty())
        _debug_variables.resize_to_defaulted(_p.debug_variables.size());
    auto& described = _debug_variables[variable];
    if (described.handle == 0)
    {
        auto place = _p.lines.place_of(v.statement);
        described.handle = debug_handle();
        described.scope
            = place.file >= 0 && place.file < i32(_p.lines.names.size()) ? debug_scope_of(place) : debug_scope(-1, -1);
    }

    auto const held = value_ref(operands[1]);
    // a value a pass removed is optimized out from here, which nothing is stated for yet
    if (!is_valid(held))
        return;
    if (i.code == op::debug_declare)
    {
        // a table global holds a constant, which no variable of the function names
        if (!is_local(held))
            return;
        debug_line_before(index);
        debug_instruction(_code, _debug_code_patches, debug::declare, {described.handle, id_of(held), _debug_expression});
        return;
    }
    if (is_global(held) || is_local(held))
        return;
    if (!is_constant(held))
    {
        auto const at = index_of(held);
        if (is_debug(_p.instructions[at].code) || (at < _unwritten.size() && _unwritten[at]))
            return;
    }
    auto indexes = cc::vector<u32>();
    if (!debug_indexes(v.type, operands[2], _p.type_of(held), indexes))
        return;
    debug_line_before(index);
    auto words = cc::vector<u32>();
    words.push_back(described.handle);
    words.push_back(id_of(held));
    words.push_back(_debug_expression);
    for (auto const k : indexes)
        words.push_back(debug_literal(k));
    debug_instruction(_code, _debug_code_patches, debug_handle(), debug::value, words);
}

u32 writer::leaves_of(type_ref t) const
{
    if (!is_valid(t))
        return 0;
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::bool_:
    case type_kind::int_:
    case type_kind::float_:
        return 1;
    case type_kind::vector:
    case type_kind::matrix:
    case type_kind::array:
        return leaves_of(info.element) * info.count;
    case type_kind::structure:
    {
        auto count = u32(0);
        for (auto m = info.first_member; m < info.first_member + info.member_count; ++m)
            count += leaves_of(_p.members[m].type);
        return count;
    }
    default:
        return 0;
    }
}

bool writer::debug_indexes(type_ref t, u32 leaf, type_ref part, cc::vector<u32>& indexes) const
{
    if (t == part && leaf == 0)
        return true;
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::vector:
    case type_kind::matrix:
    case type_kind::array:
    {
        auto const each = leaves_of(info.element);
        if (each == 0 || leaf / each >= info.count)
            return false;
        indexes.push_back(leaf / each);
        return debug_indexes(info.element, leaf % each, part, indexes);
    }
    case type_kind::structure:
        for (auto m = info.first_member; m < info.first_member + info.member_count; ++m)
        {
            auto const each = leaves_of(_p.members[m].type);
            if (leaf < each)
            {
                indexes.push_back(m - info.first_member);
                return debug_indexes(_p.members[m].type, leaf, part, indexes);
            }
            leaf -= each;
        }
        return false;
    default:
        return false;
    }
}

u32 writer::debug_type(type_ref t)
{
    if (!is_valid(t))
        return 0;
    if (_debug_types.empty())
        _debug_types.resize_to_defaulted(_p.types.size());
    if (_debug_types[isize(t)].is_made)
        return _debug_types[isize(t)].handle;

    auto& out = _debug_type_words;
    auto& patches = _debug_type_patches;
    auto const& info = _p.at(t);
    auto const bits_of = [&](auto const& self, type_ref of) -> u32
    {
        auto const& o = _p.at(of);
        switch (o.kind)
        {
        case type_kind::vector:
        case type_kind::matrix:
        case type_kind::array:
            return self(self, o.element) * o.count;
        case type_kind::structure:
        {
            auto bits = u32(0);
            for (auto m = o.first_member; m < o.first_member + o.member_count; ++m)
                bits += self(self, _p.members[m].type);
            return bits;
        }
        default:
            // a bool takes a word, as HLSL stores one
            return o.kind == type_kind::bool_ || o.width == 0 ? 32u : u32(o.width);
        }
    };

    auto result = u32(0);
    switch (info.kind)
    {
    case type_kind::bool_:
    case type_kind::int_:
    case type_kind::float_:
    {
        auto const encoding = info.kind == type_kind::bool_  ? k_encoding_boolean
                            : info.kind == type_kind::float_ ? k_encoding_float
                            : info.is_signed                 ? k_encoding_signed
                                                             : k_encoding_unsigned;
        result = debug_handle();
        u32 const words[] = {debug_string(scalar_name(info)), debug_literal(bits_of(bits_of, t)),
                             debug_literal(encoding), debug_literal(0)};
        debug_instruction(out, patches, result, debug::type_basic, words);
        break;
    }
    case type_kind::vector:
    {
        auto const element = debug_type(info.element);
        if (element == 0)
            return 0;
        result = debug_handle();
        u32 const words[] = {element, debug_literal(info.count)};
        debug_instruction(out, patches, result, debug::type_vector, words);
        break;
    }
    case type_kind::matrix:
    {
        // columns of a column vector each, as SGL and SPIR-V hold a matrix
        auto const column = debug_type(info.element);
        if (column == 0)
            return 0;
        result = debug_handle();
        u32 const words[] = {column, debug_literal(info.count), debug_literal(1)};
        debug_instruction(out, patches, result, debug::type_matrix, words);
        break;
    }
    case type_kind::array:
    {
        auto const element = debug_type(info.element);
        if (element == 0)
            return 0;
        result = debug_handle();
        u32 const words[] = {element, debug_literal(info.count)};
        debug_instruction(out, patches, result, debug::type_array, words);
        break;
    }
    case type_kind::structure:
    {
        auto members = cc::vector<u32>();
        auto offset = u32(0);
        for (auto m = info.first_member; m < info.first_member + info.member_count; ++m)
        {
            auto const& held = _p.members[m];
            auto const type = debug_type(held.type);
            if (type == 0)
                return 0;
            auto const bits = bits_of(bits_of, held.type);
            members.push_back(debug_handle());
            u32 const words[] = {debug_string(_p.name_at(held.name_first, held.name_size)),
                                 type,
                                 _debug_unit_source,
                                 debug_literal(0),
                                 debug_literal(0),
                                 debug_literal(offset),
                                 debug_literal(bits),
                                 debug_literal(k_flag_public)};
            debug_instruction(out, patches, members.back(), debug::type_member, words);
            offset += bits;
        }
        auto const name = debug_string(_p.name_at(info.name_first, info.name_size));
        auto words = cc::vector<u32>();
        words.push_back(name);
        words.push_back(debug_literal(k_composite_structure));
        words.push_back(_debug_unit_source);
        words.push_back(debug_literal(0));
        words.push_back(debug_literal(0));
        words.push_back(_debug_unit);
        words.push_back(name);
        words.push_back(debug_literal(offset));
        words.push_back(debug_literal(k_flag_public));
        words.push_back_range(members);
        result = debug_handle();
        debug_instruction(out, patches, result, debug::type_composite, words);
        break;
    }
    default:
        return 0;
    }
    _debug_types[isize(t)] = {.handle = result, .is_made = true};
    return result;
}

void writer::declare_debug_info()
{
    auto const& lines = _p.lines;
    auto const file_count = i32(lines.names.size());
    if (_debug_sources.empty())
        _debug_sources.resize_to_filled(file_count, 0u);

    // the entry point's file: where its own body stands, or else where its first scope does
    auto const entry = debug_scope(-1, -1);
    auto entry_file = i32(-1);
    for (auto const& s : _debug_scopes)
        if (s.handle == entry)
            entry_file = s.file;
    for (auto const& s : _debug_scopes)
        if (entry_file < 0)
            entry_file = s.file;
    if (entry_file < 0 || entry_file >= file_count)
        entry_file = 0;
    if (_debug_sources[entry_file] == 0)
        _debug_sources[entry_file] = debug_handle();
    auto const source_of = [&](i32 file)
    {
        if (file < 0 || file >= file_count)
            file = entry_file;
        if (_debug_sources[file] == 0)
            _debug_sources[file] = debug_handle();
        return _debug_sources[file];
    };

    auto body = cc::vector<u32>();
    auto body_patches = cc::vector<isize>();
    debug_instruction(body, body_patches, _debug_expression, debug::expression, cc::span<u32 const>());
    // a variable's file needs its source, so each is made before the sources are written
    for (auto k = isize(0); k < _debug_variables.size(); ++k)
        if (_debug_variables[k].handle != 0)
            (void)source_of(lines.place_of(_p.debug_variables[k].statement).file);
    for (auto const& s : _debug_scopes)
        (void)source_of(s.file);
    for (auto f = i32(0); f < file_count; ++f)
    {
        if (_debug_sources[f] == 0)
            continue;
        auto const text = cc::string_view(lines.texts[f]);
        auto const first = text.subview({.offset = 0, .size = cc::min(text.size(), k_source_chunk)});
        u32 const words[] = {debug_string(lines.names[f]), debug_string(first)};
        debug_instruction(body, body_patches, _debug_sources[f], debug::source, words);
        for (auto from = first.size(); from < text.size(); from += k_source_chunk)
        {
            auto const piece = text.subview({.offset = from, .size = cc::min(text.size() - from, k_source_chunk)});
            u32 const more[] = {debug_string(piece)};
            debug_instruction(body, body_patches, debug_handle(), debug::source_continued, more);
        }
    }
    {
        u32 const words[] = {debug_literal(k_debug_info_version), debug_literal(k_dwarf_version),
                             _debug_sources[entry_file], debug_literal(k_language_unknown)};
        debug_instruction(body, body_patches, _debug_unit, debug::compilation_unit, words);
    }
    append_patched(body, body_patches, _debug_type_words, _debug_type_patches);

    auto const function_type = debug_handle();
    {
        u32 const words[] = {debug_literal(k_flag_public), id_of(_void)};
        debug_instruction(body, body_patches, function_type, debug::type_function, words);
    }
    for (auto const& s : _debug_scopes)
    {
        auto const name = s.function == -1 ? cc::string_view(_p.entry_name)
                        : s.function >= 0 && s.function < lines.functions.size()
                            ? cc::string_view(lines.functions[s.function])
                            : cc::string_view("<inlined>");
        auto const line = debug_literal(u32(cc::max(s.line, 1)));
        u32 const words[] = {debug_string(name),
                             function_type,
                             source_of(s.file),
                             line,
                             debug_literal(1),
                             _debug_unit,
                             debug_string(name),
                             debug_literal(k_flag_public),
                             line};
        debug_instruction(body, body_patches, s.handle, debug::function, words);
    }
    for (auto const& at : _debug_inlined)
    {
        if (at.parent != 0)
        {
            u32 const words[] = {debug_literal(u32(at.line)), at.scope, at.parent};
            debug_instruction(body, body_patches, at.handle, debug::inlined_at, words);
        }
        else
        {
            u32 const words[] = {debug_literal(u32(at.line)), at.scope};
            debug_instruction(body, body_patches, at.handle, debug::inlined_at, words);
        }
    }
    auto parameters = u32(0);
    for (auto k = isize(0); k < _debug_variables.size(); ++k)
    {
        auto const& described = _debug_variables[k];
        if (described.handle == 0)
            continue;
        auto const& v = _p.debug_variables[k];
        auto const place = lines.place_of(v.statement);
        auto words = cc::vector<u32>();
        words.push_back(debug_string(_p.name_at(v.name_first, v.name_size)));
        words.push_back(debug_type(v.type));
        words.push_back(source_of(place.file));
        words.push_back(debug_literal(u32(cc::max(place.line, 1))));
        words.push_back(debug_literal(1));
        words.push_back(described.scope);
        words.push_back(debug_literal(k_flag_local));
        if (v.is_parameter)
            words.push_back(debug_literal(++parameters));
        debug_instruction(body, body_patches, described.handle, debug::local_variable, words);
    }

    // each handle its id, past every other; a literal the module states already is that constant
    auto ids = cc::vector<u32>::create_filled(_debug_handles, 0u);
    auto const handle_index = [](u32 h) { return isize(h & ~k_debug_handle); };
    auto reused = cc::vector<bool>::create_filled(_debug_literals.size(), false);
    for (auto k = isize(0); k < _debug_literals.size(); ++k)
    {
        auto const value = _debug_literals[k].value;
        auto& id = ids[handle_index(_debug_literals[k].id)];
        for (auto const& known : _literals)
            if (known.value == value)
                id = known.id;
        for (auto c = isize(0); c < _p.constants.size() && id == 0; ++c)
            if (auto const& held = _p.constants[c];
                held.type == _uint && !held.is_null && !held.is_undefined && held.low == value)
                id = _constants + u32(c);
        reused[k] = id != 0;
    }
    ids[handle_index(_debug_unit_source)] = 1; // an alias, resolved below
    ids[handle_index(_debug_set)] = _bound++;
    for (auto& id : ids)
        if (id == 0)
            id = _bound++;
    ids[handle_index(_debug_unit_source)] = ids[handle_index(_debug_sources[entry_file])];
    auto const resolve = [&](u32 w) { return (w & k_debug_handle) != 0 ? ids[handle_index(w)] : w; };

    // the constants the debug info names, and the `uint` they are where the module has none
    auto uint_id = is_valid(_uint) ? id_of(_uint) : 0u;
    if (uint_id == 0)
    {
        uint_id = _bound++;
        emit(_debug_declarations, spv::TypeInt, {uint_id, 32u, 0u});
    }
    for (auto k = isize(0); k < _debug_literals.size(); ++k)
        if (!reused[k])
            emit(_debug_declarations, spv::Constant,
                 {uint_id, ids[handle_index(_debug_literals[k].id)], _debug_literals[k].value});
    for (auto const at : body_patches)
        body[at] = resolve(body[at]);
    _debug_declarations.push_back_range(body);
    for (auto const at : _debug_code_patches)
        _code[at] = resolve(_code[at]);
    for (auto k = isize(0); k < _debug_string_texts.size(); ++k)
    {
        head(_sources, spv::String, 2 + string_words(_debug_string_texts[k]));
        _sources.push_back(resolve(_debug_string_handles[k]));
        string_into(_sources, _debug_string_texts[k]);
    }
    _debug_set_id = resolve(_debug_set);
}
