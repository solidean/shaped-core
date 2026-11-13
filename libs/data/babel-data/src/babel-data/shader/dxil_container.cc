#include "dxil_container.hh"

#include <clean-core/string/format.hh>

namespace babel::dxil
{
namespace
{
[[nodiscard]] u32 word_at(cc::span<byte const> b, isize at)
{
    return u32(u8(b[at])) | (u32(u8(b[at + 1])) << 8) | (u32(u8(b[at + 2])) << 16) | (u32(u8(b[at + 3])) << 24);
}

// the header: magic, a 16-byte digest, the version, the file's size, the part count, then each part's offset
constexpr isize k_header_bytes = 32;
} // namespace

part const* container::part_named(cc::string_view name) const
{
    for (auto const& p : parts)
        if (p.name() == name)
            return &p;
    return nullptr;
}

cc::result<container> read(cc::span<byte const> bytes)
{
    if (bytes.size() < k_header_bytes || bytes[0] != byte('D') || bytes[1] != byte('X') || bytes[2] != byte('B')
        || bytes[3] != byte('C'))
        return cc::error("dxil: no `DXBC` container header");
    auto c = container();
    c.bytes = cc::vector<byte>::create_copy_of(bytes);
    auto const b = cc::span<byte const>(c.bytes);
    c.version_major = u16(u8(b[20])) | u16(u16(u8(b[21])) << 8);
    c.version_minor = u16(u8(b[22])) | u16(u16(u8(b[23])) << 8);
    auto const count = isize(word_at(b, 28));
    if (k_header_bytes + count * 4 > b.size())
        return cc::error(cc::format("dxil: {} part offsets run past the end", count));
    for (auto k = isize(0); k < count; ++k)
    {
        auto const at = isize(word_at(b, k_header_bytes + k * 4));
        if (at + 8 > b.size())
            return cc::error(cc::format("dxil: part {} starts past the end", k));
        auto const size = isize(word_at(b, at + 4));
        if (at + 8 + size > b.size())
            return cc::error(cc::format("dxil: part {} runs {} bytes past the end", k, at + 8 + size - b.size()));
        auto p = part{.data = b.subspan({.offset = at + 8, .size = size})};
        for (auto i = 0; i < 4; ++i)
            p.code[i] = char(b[at + i]);
        c.parts.push_back(p);
    }
    return c;
}

cc::result<program> program_of(part const& p)
{
    // the program header: version, size in words; then the DXIL header: `DXIL`, its version, the bitcode's offset
    // from that header, the bitcode's size
    auto const d = p.data;
    if (d.size() < 24)
        return cc::error(cc::format("dxil: a {} part of {} bytes is short of a program header", p.name(), d.size()));
    auto const version = word_at(d, 0);
    if (d[8] != byte('D') || d[9] != byte('X') || d[10] != byte('I') || d[11] != byte('L'))
        return cc::error(cc::format("dxil: the {} part's program header has no `DXIL` magic", p.name()));
    auto const offset = isize(word_at(d, 16));
    auto const size = isize(word_at(d, 20));
    if (8 + offset + size > d.size())
        return cc::error(cc::format("dxil: the {} part's bitcode runs past the part", p.name()));
    return program{.shader_kind = version >> 16,
                   .model_major = (version >> 4) & 0xfu,
                   .model_minor = version & 0xfu,
                   .dxil_version = word_at(d, 12),
                   .bitcode = d.subspan({.offset = 8 + offset, .size = size})};
}
} // namespace babel::dxil
