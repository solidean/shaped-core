#include "writer.hh"

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil;
using namespace sgl::ir::dxil::impl;

// A DXBC container: a header, the offset of each part, then the parts.
// A part is a row of `parts_of`, so a new one is a function and a row; a stage's signatures fill the ones compute leaves
// empty.

namespace
{
[[nodiscard]] constexpr u32 four_cc(char a, char b, char c, char d)
{
    return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

constexpr u32 k_triple_dxbc = four_cc('D', 'X', 'B', 'C');
/// What the epic's spike found the retail runtime takes in place of a validator's hash.
constexpr u8 k_bypass_digest = 0x01;
constexpr u32 k_bitcode_offset = 16;

class byte_writer
{
public:
    void u32_le(u32 value)
    {
        for (auto i = 0; i < 4; ++i)
            _bytes.push_back(byte((value >> (i * 8)) & 0xffu));
    }
    void u16_le(u16 value)
    {
        _bytes.push_back(byte(value & 0xffu));
        _bytes.push_back(byte(value >> 8));
    }
    void u8_le(u8 value) { _bytes.push_back(byte(value)); }
    void append(cc::span<byte const> bytes)
    {
        for (auto const b : bytes)
            _bytes.push_back(b);
    }
    void zeros(isize count)
    {
        for (auto i = isize(0); i < count; ++i)
            _bytes.push_back(byte(0));
    }
    [[nodiscard]] isize size() const { return _bytes.size(); }
    [[nodiscard]] cc::vector<byte> take() { return cc::move(_bytes); }

private:
    cc::vector<byte> _bytes;
};

/// `PSVShaderKind`.
constexpr u32 k_psv_geometry = 2;
constexpr u32 k_psv_hull = 3;
constexpr u32 k_psv_domain = 4;
constexpr u32 k_psv_mesh = 13;
constexpr u32 k_psv_task = 14;

/// SFI0: the features a pipeline must be able to give it.
[[nodiscard]] cc::vector<byte> feature_part(module_summary const& s)
{
    auto out = byte_writer();
    out.u32_le(u32(s.feature_flags));
    out.u32_le(u32(s.feature_flags >> 32));
    return out.take();
}

/// Pads `out` to a word, as a part's string table ends.
void pad_to_word(byte_writer& out)
{
    out.zeros(((out.size() + 3) & ~isize(3)) - out.size());
}

/// ISG1 and OSG1: the header, a 32-byte record per element, then each name once.
/// A validator rebuilds this from the module and compares bytes, so nothing here is free to differ from DXC's layout.
[[nodiscard]] cc::vector<byte> signature_part(module_summary const& s, signature_table table)
{
    constexpr auto k_header_size = u32(8);
    constexpr auto k_element_size = u32(32);
    auto elements = cc::vector<isize>();
    for (auto i = isize(0); i < s.signature.size(); ++i)
        if (s.signature[i].table == table)
            elements.push_back(i);
    // names first, as they are declared, since the order below reads their offsets
    auto names = cc::vector<cc::string_view>();
    auto offsets = cc::vector<u32>();
    auto next = k_header_size + u32(elements.size()) * k_element_size;
    auto const offset_of = [&](cc::string_view name)
    {
        for (auto n = isize(0); n < names.size(); ++n)
            if (names[n] == name)
                return offsets[n];
        names.push_back(name);
        offsets.push_back(next);
        next += u32(name.size()) + 1;
        return offsets.back();
    };
    for (auto const i : elements)
        (void)offset_of(s.signature[i].name);

    // by register then name offset, where `PSV0` and `dx.sig` keep declared order; a no-row element's -1 sorts last
    // One name in a row stays declared, which is by column: DXC's sort reads the offset, so a factor leads a link in `w`.
    auto const place_of = [&](isize i) { return (u64(u32(s.signature[i].row)) << 32) | offset_of(s.signature[i].name); };
    for (auto a = isize(1); a < elements.size(); ++a)
    {
        auto const held = elements[a];
        auto at = a;
        for (; at > 0 && place_of(elements[at - 1]) > place_of(held); --at)
            elements[at] = elements[at - 1];
        elements[at] = held;
    }

    auto out = byte_writer();
    out.u32_le(u32(elements.size()));
    out.u32_le(k_header_size);
    for (auto const i : elements)
    {
        auto const& e = s.signature[i];
        auto const used = u8(s.usage[i] << e.first_column);
        out.u32_le(0); // the stream
        out.u32_le(offset_of(e.name));
        out.u32_le(e.semantic_index);
        out.u32_le(e.system_value);
        out.u32_le(e.component_type);
        out.u32_le(u32(e.row));
        out.u8_le(u8(e.mask()));
        // what is read states the lanes it reads, and what is written the ones it never writes
        auto const is_read
            = table == signature_table::input || (table == signature_table::patch && s.psv_stage == k_psv_domain);
        out.u8_le(is_read ? used : u8(~used & 0xf));
        out.u16_le(0);
        out.u32_le(0); // no minimum precision
    }
    for (auto const name : names)
    {
        for (auto const c : name)
            out.u8_le(u8(c));
        out.u8_le(0);
    }
    pad_to_word(out);
    return out.take();
}

/// PSV0: what a runtime reads without parsing the module.
[[nodiscard]] cc::vector<byte> pipeline_state_part(module_summary const& s)
{
    constexpr auto k_runtime_info_size = u32(52);
    constexpr auto k_resource_size = u32(24);
    constexpr auto k_signature_element_size = u32(16);

    // the string table holds an ordinary element's name for each one, unshared, then the entry's
    auto strings = byte_writer();
    strings.u8_le(0);
    auto name_offsets = cc::vector<u32>();
    auto index_offsets = cc::vector<u32>();
    auto row_counts = cc::vector<u8>();
    auto indices = cc::vector<u32>();
    auto inputs = 0;
    auto outputs = 0;
    auto patches = 0;
    for (auto at = isize(0); at < s.signature.size(); ++at)
    {
        auto const& e = s.signature[at];
        if (e.continues)
            continue;
        (e.table == signature_table::input ? inputs : e.table == signature_table::output ? outputs : patches) += 1;
        auto name_at = u32(0);
        if (e.system_value == 0)
        {
            name_at = u32(strings.size());
            for (auto const c : cc::string_view(e.name))
                strings.u8_le(u8(c));
            strings.u8_le(0);
        }
        name_offsets.push_back(name_at);
        // the run of indices it states, where a run that stands already is shared
        auto rows = isize(1);
        while (at + rows < s.signature.size() && s.signature[at + rows].continues)
            ++rows;
        auto const matches = [&](isize from)
        {
            for (auto k = isize(0); k < rows; ++k)
                if (from + k >= indices.size() || indices[from + k] != s.signature[at + k].semantic_index)
                    return false;
            return true;
        };
        auto index_at = isize(0);
        while (index_at < indices.size() && !matches(index_at))
            ++index_at;
        if (index_at == indices.size())
            for (auto k = isize(0); k < rows; ++k)
                indices.push_back(s.signature[at + k].semantic_index);
        index_offsets.push_back(u32(index_at));
        row_counts.push_back(u8(rows));
    }
    auto const entry_at = u32(strings.size());
    for (auto const c : cc::string_view(s.entry_name))
        strings.u8_le(u8(c));
    strings.u8_le(0);
    pad_to_word(strings);

    auto out = byte_writer();
    out.u32_le(k_runtime_info_size);
    // PSVRuntimeInfo0: a stage's own 16 bytes, then the wave sizes
    auto const stage_info_at = out.size();
    if (s.psv_stage == k_psv_geometry)
    {
        out.u32_le(s.input_primitive);
        out.u32_le(s.output_topology);
        out.u32_le(1); // the one stream
        out.u8_le(s.writes_position ? 1 : 0);
    }
    else if (s.psv_stage == k_psv_hull)
    {
        out.u32_le(s.control_points);
        out.u32_le(s.control_points);
        out.u32_le(s.domain);
        out.u32_le(s.output_primitive);
    }
    else if (s.psv_stage == k_psv_domain)
    {
        out.u32_le(s.control_points);
        out.u8_le(s.writes_position ? 1 : 0);
        out.zeros(3);
        out.u32_le(s.domain);
    }
    else if (s.psv_stage == k_psv_mesh)
    {
        out.u32_le(s.workgroup_bytes);
        out.u32_le(0); // what of it depends on the view
        out.u32_le(s.payload_bytes);
        out.u16_le(u16(s.max_vertices));
        out.u16_le(u16(s.max_primitives));
    }
    else if (s.psv_stage == k_psv_task)
        out.u32_le(s.payload_bytes);
    else
    {
        out.u8_le(s.writes_position || s.writes_depth ? 1 : 0);
        out.u8_le(s.runs_per_sample ? 1 : 0);
    }
    out.zeros(16 - (out.size() - stage_info_at));
    out.u32_le(0);
    out.u32_le(0xffffffffu);
    // PSVRuntimeInfo1
    out.u8_le(u8(s.psv_stage));
    out.u8_le(s.uses_view ? 1 : 0);
    // a geometry stage's most vertices, or the rows of patch constants, beside a mesh stage's topology
    out.u16_le(u16(s.psv_stage == k_psv_geometry ? s.max_vertices : s.patch_rows | (s.mesh_topology << 8)));
    out.u8_le(u8(inputs));
    out.u8_le(u8(outputs));
    out.u8_le(u8(patches));
    out.u8_le(u8(s.input_rows));
    out.u8_le(u8(s.output_rows));
    out.zeros(3); // the other three streams
    // PSVRuntimeInfo2: the thread group
    for (auto axis = 0; axis < 3; ++axis)
        out.u32_le(u32(s.workgroup[axis]));
    // PSVRuntimeInfo3: where the entry's name stands in the string table
    out.u32_le(entry_at);

    out.u32_le(u32(s.resources.size()));
    if (!s.resources.empty())
    {
        out.u32_le(k_resource_size);
        for (auto const& r : s.resources)
        {
            out.u32_le(r.psv_type);
            out.u32_le(r.space);
            out.u32_le(r.lower);
            out.u32_le(r.upper);
            out.u32_le(r.kind);
            out.u32_le(r.psv_flags);
        }
    }

    auto const string_bytes = strings.take();
    out.u32_le(u32(string_bytes.size()));
    out.append(string_bytes);
    out.u32_le(u32(indices.size()));
    for (auto const index : indices)
        out.u32_le(index);
    if (s.signature.empty())
        return out.take();

    out.u32_le(k_signature_element_size);
    auto stated = isize(0);
    for (auto i = isize(0); i < s.signature.size(); ++i)
    {
        auto const& e = s.signature[i];
        if (e.continues)
            continue;
        out.u32_le(name_offsets[stated]);
        out.u32_le(index_offsets[stated]);
        auto const has_row = e.row >= 0;
        out.u8_le(row_counts[stated]);
        ++stated;
        out.u8_le(has_row ? u8(e.row) : u8(0));
        // the columns, then the first one and that it is allocated, which an element with no register is not
        out.u8_le(u8(e.lanes | (has_row ? (e.first_column << 4) | 0x40 : 0)));
        out.u8_le(s.semantic_kinds[i]);
        out.u8_le(u8(e.component_type));
        out.u8_le(s.modes[i]);
        out.u8_le(0); // no dynamic index, the first stream
        out.u8_le(0);
    }
    for (auto const word : s.view_outputs)
        out.u32_le(word);
    for (auto const word : s.view_patch)
        out.u32_le(word);
    for (auto const word : s.dependencies)
        out.u32_le(word);
    for (auto const word : s.patch_dependencies)
        out.u32_le(word);
    return out.take();
}

[[nodiscard]] cc::vector<byte> program_part(module_summary const& s, cc::span<byte const> bitcode)
{
    auto out = byte_writer();
    out.u32_le((s.program_kind << 16) | s.program_version);
    out.u32_le(u32((bitcode.size() + 24) / 4));
    out.u32_le(four_cc('D', 'X', 'I', 'L'));
    out.u32_le(s.dxil_version);
    out.u32_le(k_bitcode_offset);
    out.u32_le(u32(bitcode.size()));
    out.append(bitcode);
    return out.take();
}

constexpr cc::string_view k_triple = "dxil-ms-dx";
constexpr cc::string_view k_layout = "e-m:e-p:32:32-i1:32-i8:8-i16:16-i32:32-i64:64-f16:16-f32:32-f64:64-n8:16:32:64";
struct container_part
{
    u32 name;
    cc::vector<byte> bytes;
};

[[nodiscard]] cc::vector<byte> assembled(cc::span<container_part const> parts)
{
    auto header = u32(32 + parts.size() * 4);
    auto total = header;
    for (auto const& p : parts)
        total += 8 + u32(p.bytes.size());

    auto out = byte_writer();
    out.u32_le(k_triple_dxbc);
    for (auto i = 0; i < 16; ++i)
        out.u8_le(k_bypass_digest);
    out.u16_le(1);
    out.u16_le(0);
    out.u32_le(total);
    out.u32_le(u32(parts.size()));
    auto offset = header;
    for (auto const& p : parts)
    {
        out.u32_le(offset);
        offset += 8 + u32(p.bytes.size());
    }
    for (auto const& p : parts)
    {
        out.u32_le(p.name);
        out.u32_le(u32(p.bytes.size()));
        out.append(p.bytes);
    }
    return out.take();
}

/// `RDAT`: the tables a state object reads a library by, which the validator builds again and compares.
[[nodiscard]] cc::vector<byte> runtime_data_part(module_summary const& s)
{
    constexpr u32 k_part_strings = 1;
    constexpr u32 k_part_index_arrays = 2;
    constexpr u32 k_part_resources = 3;
    constexpr u32 k_part_functions = 4;
    constexpr u32 k_version = 0x10;
    constexpr u32 k_feature_64_uavs = 1u << 3;

    auto strings = byte_writer();
    strings.u8_le(0);
    auto const add_string = [&](cc::string_view text)
    {
        auto const at = u32(strings.size());
        for (auto const c : text)
            strings.u8_le(u8(c));
        strings.u8_le(0);
        return at;
    };

    auto resources = byte_writer();
    resources.u32_le(u32(s.resources.size()));
    resources.u32_le(32);
    for (auto const& r : s.resources)
    {
        resources.u32_le(r.cls);
        resources.u32_le(r.kind);
        resources.u32_le(r.id);
        resources.u32_le(r.space);
        resources.u32_le(r.lower);
        resources.u32_le(r.upper);
        resources.u32_le(add_string(r.name));
        resources.u32_le(r.flags);
    }
    auto const name = add_string(s.entry_name);
    pad_to_word(strings);

    auto functions = byte_writer();
    functions.u32_le(1);
    functions.u32_le(52);
    functions.u32_le(name);
    functions.u32_le(name);
    functions.u32_le(s.resources.empty() ? 0xffffffffu : 0u);
    functions.u32_le(0xffffffffu);
    functions.u32_le(s.shader_kind);
    functions.u32_le(s.payload_bytes);
    functions.u32_le(s.attribute_bytes);
    // how many UAVs a module holds is the module's, and a function states what it uses itself
    functions.u32_le(u32(s.feature_flags) & ~k_feature_64_uavs);
    functions.u32_le(u32(s.feature_flags >> 32));
    functions.u32_le(1u << s.shader_kind);
    functions.u32_le((s.shader_kind << 16) | 0x60 | s.min_shader_minor);
    functions.u32_le(0);
    functions.u32_le(0xffffffffu);

    auto indices = byte_writer();
    indices.u32_le(u32(s.resources.size()));
    for (auto i = u32(0); i < u32(s.resources.size()); ++i)
        indices.u32_le(i);

    struct table
    {
        u32 kind;
        cc::vector<byte> bytes;
    };
    auto tables = cc::vector<table>();
    tables.push_back({k_part_strings, strings.take()});
    if (!s.resources.empty())
        tables.push_back({k_part_resources, resources.take()});
    tables.push_back({k_part_functions, functions.take()});
    if (!s.resources.empty())
        tables.push_back({k_part_index_arrays, indices.take()});

    auto out = byte_writer();
    out.u32_le(k_version);
    out.u32_le(u32(tables.size()));
    auto offset = u32(8 + tables.size() * 4);
    for (auto const& t : tables)
    {
        out.u32_le(offset);
        offset += 8 + u32(t.bytes.size());
    }
    for (auto const& t : tables)
    {
        out.u32_le(t.kind);
        out.u32_le(u32(t.bytes.size()));
        out.append(t.bytes);
    }
    return out.take();
}

/// EMIT-257: the name a debugger looks the program's debug info up by, NUL-ended and padded to a word.
[[nodiscard]] cc::vector<byte> debug_name_part(cc::string_view name)
{
    auto out = byte_writer();
    out.u16_le(0); // no flags
    out.u16_le(u16(name.size()));
    for (auto const c : name)
        out.u8_le(u8(c));
    out.u8_le(0);
    auto const written = 4 + name.size() + 1;
    out.zeros((4 - written % 4) % 4);
    return out.take();
}

} // namespace

cc::vector<byte> sgl::ir::dxil::impl::write_container(writer const& w)
{
    auto const bitcode = w.module().write(k_triple, k_layout);
    auto const& s = w.summary();
    // DXC's -Zi: the program part keeps its bytes, and the debug module beside it carries the lines
    auto const add_debug_parts = [&](cc::vector<container_part>& parts)
    {
        if (!w.has_debug_info())
            return;
        auto const debug = w.module().write(k_triple, k_layout, true);
        parts.push_back({.name = four_cc('I', 'L', 'D', 'B'), .bytes = program_part(s, debug)});
        parts.push_back({.name = four_cc('I', 'L', 'D', 'N'), .bytes = debug_name_part(w.debug_name())});
    };

    auto parts = cc::vector<container_part>();
    parts.push_back({.name = four_cc('S', 'F', 'I', '0'), .bytes = feature_part(s)});
    if (s.is_library)
    {
        parts.push_back({.name = four_cc('R', 'D', 'A', 'T'), .bytes = runtime_data_part(s)});
        add_debug_parts(parts);
        parts.push_back({.name = four_cc('D', 'X', 'I', 'L'), .bytes = program_part(s, bitcode)});
        return assembled(parts);
    }
    parts.push_back({.name = four_cc('I', 'S', 'G', '1'), .bytes = signature_part(s, signature_table::input)});
    parts.push_back({.name = four_cc('O', 'S', 'G', '1'), .bytes = signature_part(s, signature_table::output)});
    // a stage with patch constants has the third, though it reads none; a mesh stage only where it writes per primitive
    auto const writes_primitives = s.psv_stage == k_psv_mesh && s.patch_rows > 0;
    if (s.psv_stage == k_psv_hull || s.psv_stage == k_psv_domain || writes_primitives)
        parts.push_back({.name = four_cc('P', 'S', 'G', '1'), .bytes = signature_part(s, signature_table::patch)});
    parts.push_back({.name = four_cc('P', 'S', 'V', '0'), .bytes = pipeline_state_part(s)});
    add_debug_parts(parts);
    parts.push_back({.name = four_cc('D', 'X', 'I', 'L'), .bytes = program_part(s, bitcode)});

    return assembled(parts);
}
