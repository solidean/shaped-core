#pragma once

#include <babel-data/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>

// DXIL container reader (shader/): the `DXBC` file DXC and SGL's own DXIL writer both produce.
//
// A container is a header and a table of parts, each a four-character code and its bytes: `DXIL` the program,
// `ILDB` the program with debug info, `ISG1` / `OSG1` / `PSG1` the signatures, `PSV0` the pipeline state, `HASH`,
// `SFI0`, `STAT`, `RDAT`.
// Nothing inside a part is decoded but the program's own header, which says what shader it is and where its LLVM
// bitcode starts; babel::llvm::read_bitstream reads that bitcode, and babel::llvm37 what it means.
//
//   auto const c = babel::dxil::read(bytes).value();
//   auto const* const program = c.part_named("DXIL");           // null where the container holds none
//   auto const p = babel::dxil::program_of(*program).value();
//   p.shader_kind;                                         // 5 for compute
//   auto const bc = babel::llvm::read_bitstream(p.bitcode).value();

/// One part: its code and its bytes, a view of the container's.
struct babel::dxil::part
{
    /// `DXIL`, `PSV0`: the code's four characters, which need not be letters.
    char code[4] = {};
    cc::span<byte const> data;

    [[nodiscard]] cc::string_view name() const { return cc::string_view(code, 4); }
};

/// What a `DXIL` or `ILDB` part's header says, and the bitcode after it.
struct babel::dxil::program
{
    /// The pipeline stage, D3D's numbering: 0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute, 6 library,
    /// 13 mesh, 14 amplification.
    u32 shader_kind = 0;
    u32 model_major = 0;
    u32 model_minor = 0;
    /// The DXIL version, major in the high byte of the low half.
    u32 dxil_version = 0;
    cc::span<byte const> bitcode;
};

/// A container, read once: its parts, each a view of `bytes`.
struct babel::dxil::container
{
    cc::vector<byte> bytes;
    cc::vector<part> parts;
    u16 version_major = 0;
    u16 version_minor = 0;

    /// The first part of code `name`, or null.
    [[nodiscard]] part const* part_named(cc::string_view name) const;
};

namespace babel::dxil
{
/// An error where the magic is not `DXBC`, or a part runs past the end.
[[nodiscard]] cc::result<container> read(cc::span<byte const> bytes);

/// The program a `DXIL` or `ILDB` part holds; an error where its header is short or its bitcode runs past the part.
[[nodiscard]] cc::result<program> program_of(part const& p);
} // namespace babel::dxil
