#include "writer.hh"

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil::impl;

// EMIT-257: the debug info of DXC's -Zi, as far as a line table needs it.
// One `DISubprogram` scopes the entry point's own lines, and one per file the lines inlined from it.
// No variable is described: the table maps operations to lines and to the calls they came through.

namespace
{
// LLVM 3.7's metadata records
constexpr u32 k_di_location = 7;
constexpr u32 k_di_file = 16;
constexpr u32 k_di_subroutine_type = 19;
constexpr u32 k_di_compile_unit = 20;
constexpr u32 k_di_subprogram = 21;

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
    // what a file's inlined lines stand in, which names no function of the module
    auto inlined = cc::vector<u32>::create_filled(file_count, 0u);
    auto const inlined_scope = [&](i32 f)
    {
        if (inlined[f] == 0)
        {
            inlined[f] = subprogram(_m.md_string("<inlined>"), f, 0);
            subprograms.push_back(inlined[f]);
        }
        return inlined[f];
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
            auto const scope = k == place.calls_count ? entry : inlined_scope(call.file);
            // a location record names its scope by its position, unlike every other operand here
            u64 const fields[] = {1, u64(call.line), 0, u64(scope - 1), at};
            at = _m.md_record(k_di_location, fields);
        }
        auto const scope = place.calls_count == 0 && place.file == entry_file ? entry : inlined_scope(place.file);
        locations[s] = {.line = u32(place.line), .column = 0, .scope = scope, .inlined_at = at};
    }
    _m.set_debug_locations(cc::move(locations));

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
