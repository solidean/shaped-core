#pragma once

#include <babel-data/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string_view.hh>

// SPIR-V module reader (shader/).
//
// The format-shaped view of a module: its header, and its instructions as words, in the order the module states them.
// Nothing is decoded beyond the shape every instruction shares: an opcode, a word count, and the words after them.
// What an operand means is the opcode's to say; `info_of` knows whether an opcode has a result id and a result type,
// from a table generated out of the SPIR-V headers (tools/gen-spirv-opcodes.py), and nothing more.
//
// THE WORDS ARE THE MODULE'S OWN, in the host's byte order.
// A module written big-endian is swapped once on read, which the magic number tells; `header::magic` is then the
// little-endian one either way.
//
// A malformed module is an error, never a partial read: a header short of five words, an instruction of zero words, or
// one that runs past the end.
// An opcode the table does not know is no error, since an extension may add any; `info_of` returns null for it.
//
//   auto const m = babel::spirv::read(bytes).value();
//   for (auto i = isize(0); i < m.instruction_count(); ++i)
//       if (m.at(i).opcode() == 54) // OpFunction
//           ++functions;

/// The module header, as stored.
struct babel::spirv::header
{
    u32 magic = 0;
    /// `0x00MMmm00`: major and minor in the middle bytes.
    u32 version = 0;
    u32 generator = 0;
    /// Every id the module uses is below it.
    u32 bound = 0;
    u32 schema = 0;
};

/// What the SPIR-V headers say of an opcode.
struct babel::spirv::opcode_info
{
    u16 opcode = 0;
    bool has_result = false;
    bool has_result_type = false;
    /// `OpFAdd`, with its prefix, as the specification spells it.
    char const* name = "";
};

/// One instruction: a view of its words, the first of which holds its opcode and word count.
struct babel::spirv::instruction
{
    cc::span<u32 const> words;

    [[nodiscard]] u16 opcode() const { return u16(words[0] & 0xffffu); }
    /// Every word after the first.
    [[nodiscard]] cc::span<u32 const> operands() const { return words.subspan(1); }
    /// The result id, or 0 where the opcode gives none or the table does not know it.
    [[nodiscard]] u32 result_id() const;
    /// The result type's id, or 0 where the opcode takes none or the table does not know it.
    [[nodiscard]] u32 result_type() const;
    /// The operands after the result type and result id, where the table knows the opcode; all of them otherwise.
    [[nodiscard]] cc::span<u32 const> arguments() const;
    /// The literal string starting at operand `at`: its bytes up to the terminating zero, and how many words it took.
    [[nodiscard]] cc::string_view string_at(isize at, isize* words_taken = nullptr) const;
};

/// A module, read once: its header and its instructions.
class babel::spirv::module
{
public:
    header head;

    [[nodiscard]] isize instruction_count() const { return _starts.size(); }
    [[nodiscard]] instruction at(isize i) const;
    /// Every word after the header.
    [[nodiscard]] cc::span<u32 const> words() const { return _words; }
    /// The major and minor version the header states.
    [[nodiscard]] int major_version() const { return int((head.version >> 16) & 0xffu); }
    [[nodiscard]] int minor_version() const { return int((head.version >> 8) & 0xffu); }

private:
    friend cc::result<module> read(cc::span<byte const> bytes);
    cc::vector<u32> _words;
    /// Where each instruction starts in `_words`.
    cc::vector<u32> _starts;
};

namespace babel::spirv
{
/// The table's entry for `opcode`, or null for one it does not know.
[[nodiscard]] opcode_info const* info_of(u16 opcode);

[[nodiscard]] cc::result<module> read(cc::read_stream& in);
[[nodiscard]] cc::result<module> read(cc::span<byte const> bytes);
/// Words already in memory, in the host's byte order.
[[nodiscard]] cc::result<module> read(cc::span<u32 const> words);
} // namespace babel::spirv
