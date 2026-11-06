#include "writer.hh"

using namespace sgl;
using namespace sgl::ir;
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
/// A module's DXIL version, as the program header states it.
constexpr u32 k_dxil_version = 0x106;

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

/// SFI0: the features a pipeline must be able to give it.
[[nodiscard]] cc::vector<byte> feature_part(module_summary const& s)
{
    auto out = byte_writer();
    out.u32_le(u32(s.feature_flags));
    out.u32_le(u32(s.feature_flags >> 32));
    return out.take();
}

/// ISG1 and OSG1, which hold no element in a compute stage.
[[nodiscard]] cc::vector<byte> empty_signature_part()
{
    auto out = byte_writer();
    out.u32_le(0); // the elements
    out.u32_le(8); // where they would start
    return out.take();
}

/// PSV0: what a runtime reads without parsing the module.
[[nodiscard]] cc::vector<byte> pipeline_state_part(module_summary const& s)
{
    constexpr auto k_runtime_info_size = u32(52);
    constexpr auto k_resource_size = u32(24);
    auto out = byte_writer();
    out.u32_le(k_runtime_info_size);
    // PSVRuntimeInfo0: a stage's own 16 bytes, then the wave sizes
    out.zeros(16);
    out.u32_le(0);
    out.u32_le(0xffffffffu);
    // PSVRuntimeInfo1
    out.u8_le(u8(s.psv_stage));
    out.u8_le(0); // uses no view id
    out.u16_le(0);
    out.zeros(8); // the signature counts
    // PSVRuntimeInfo2: the thread group
    for (auto axis = 0; axis < 3; ++axis)
        out.u32_le(u32(s.workgroup[axis]));
    // PSVRuntimeInfo3: where the entry's name stands in the string table
    out.u32_le(1);

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

    // the string table: empty string first, padded to a word
    auto const text = s.entry_name.size() + 2;
    auto const padded = (text + 3) & ~isize(3);
    out.u32_le(u32(padded));
    out.u8_le(0);
    for (auto const c : cc::string_view(s.entry_name))
        out.u8_le(u8(c));
    out.zeros(padded - text + 1);
    out.u32_le(0); // no semantic indices
    return out.take();
}

[[nodiscard]] cc::vector<byte> program_part(module_summary const& s, cc::span<byte const> bitcode)
{
    auto out = byte_writer();
    out.u32_le((s.program_kind << 16) | s.program_version);
    out.u32_le(u32((bitcode.size() + 24) / 4));
    out.u32_le(four_cc('D', 'X', 'I', 'L'));
    out.u32_le(k_dxil_version);
    out.u32_le(k_bitcode_offset);
    out.u32_le(u32(bitcode.size()));
    out.append(bitcode);
    return out.take();
}

constexpr cc::string_view k_triple = "dxil-ms-dx";
constexpr cc::string_view k_layout = "e-m:e-p:32:32-i1:32-i8:8-i16:16-i32:32-i64:64-f16:16-f32:32-f64:64-n8:16:32:64";
} // namespace

cc::vector<byte> sgl::ir::dxil::impl::write_container(writer const& w)
{
    auto const bitcode = w.module().write(k_triple, k_layout);
    auto const& s = w.summary();

    struct part
    {
        u32 name;
        cc::vector<byte> bytes;
    };
    auto parts = cc::vector<part>();
    parts.push_back({.name = four_cc('S', 'F', 'I', '0'), .bytes = feature_part(s)});
    parts.push_back({.name = four_cc('I', 'S', 'G', '1'), .bytes = empty_signature_part()});
    parts.push_back({.name = four_cc('O', 'S', 'G', '1'), .bytes = empty_signature_part()});
    parts.push_back({.name = four_cc('P', 'S', 'V', '0'), .bytes = pipeline_state_part(s)});
    parts.push_back({.name = four_cc('D', 'X', 'I', 'L'), .bytes = program_part(s, bitcode)});

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
