#include "spirv.hh"

#include <clean-core/streams/span_stream.hh>
#include <clean-core/string/format.hh>

namespace babel::spirv::impl
{
extern opcode_info const k_opcode_table[];
extern isize const k_opcode_count;
} // namespace babel::spirv::impl

namespace babel::spirv
{
namespace
{
constexpr u32 k_magic = 0x07230203u;
constexpr u32 k_swapped_magic = 0x03022307u;
constexpr isize k_header_words = 5;

[[nodiscard]] constexpr u32 swapped(u32 w)
{
    return (w >> 24) | ((w >> 8) & 0xff00u) | ((w << 8) & 0xff0000u) | (w << 24);
}
} // namespace

opcode_info const* info_of(u16 opcode)
{
    // the table is sorted by opcode
    auto low = isize(0);
    auto high = impl::k_opcode_count;
    while (low < high)
    {
        auto const mid = (low + high) / 2;
        auto const at = impl::k_opcode_table[mid].opcode;
        if (at == opcode)
            return &impl::k_opcode_table[mid];
        if (at < opcode)
            low = mid + 1;
        else
            high = mid;
    }
    return nullptr;
}

u32 instruction::result_id() const
{
    auto const* const info = info_of(opcode());
    if (info == nullptr || !info->has_result)
        return 0;
    auto const at = isize(info->has_result_type ? 2 : 1);
    return at < words.size() ? words[at] : 0;
}

u32 instruction::result_type() const
{
    auto const* const info = info_of(opcode());
    if (info == nullptr || !info->has_result_type)
        return 0;
    return words.size() > 1 ? words[1] : 0;
}

cc::span<u32 const> instruction::arguments() const
{
    auto const* const info = info_of(opcode());
    auto skip = isize(1);
    if (info != nullptr)
        skip += (info->has_result ? 1 : 0) + (info->has_result_type ? 1 : 0);
    return skip <= words.size() ? words.subspan(skip) : cc::span<u32 const>();
}

cc::string_view instruction::string_at(isize at, isize* words_taken) const
{
    // a literal string is its UTF-8 bytes, zero-terminated and padded to a word, the first byte in the low bits
    auto const from = at + 1;
    if (from >= words.size())
    {
        if (words_taken != nullptr)
            *words_taken = 0;
        return {};
    }
    auto const* const bytes = reinterpret_cast<char const*>(words.data() + from);
    auto const most = (words.size() - from) * 4;
    auto length = isize(0);
    while (length < most && bytes[length] != '\0')
        ++length;
    if (words_taken != nullptr)
        *words_taken = cc::min(words.size() - from, length / 4 + 1);
    return cc::string_view(bytes, length);
}

instruction module::at(isize i) const
{
    auto const first = isize(_starts[i]);
    auto const count = isize(_words[first] >> 16);
    return {.words = cc::span<u32 const>(_words.data() + first, count)};
}

cc::result<module> read(cc::span<u32 const> words)
{
    auto bytes = cc::span<byte const>(reinterpret_cast<byte const*>(words.data()), words.size() * 4);
    return read(bytes);
}

cc::result<module> read(cc::span<byte const> bytes)
{
    if (bytes.size() % 4 != 0)
        return cc::error(cc::format("spirv: {} bytes is no whole number of words", bytes.size()));
    auto const count = bytes.size() / 4;
    if (count < k_header_words)
        return cc::error(cc::format("spirv: {} words is short of a header's five", count));
    // little-endian bytes, swapped once where the magic says the module was written big-endian
    auto const word_at = [&](isize i)
    {
        auto w = u32(0);
        for (auto k = 0; k < 4; ++k)
            w |= u32(u8(bytes[i * 4 + k])) << (8 * k);
        return w;
    };
    auto const is_swapped = word_at(0) == k_swapped_magic;
    if (word_at(0) != k_magic && !is_swapped)
        return cc::error(cc::format("spirv: the magic number is {:#010x}, which is no SPIR-V module's", word_at(0)));
    auto const host = [&](isize i) { return is_swapped ? swapped(word_at(i)) : word_at(i); };

    auto m = module();
    m.head = {.magic = host(0), .version = host(1), .generator = host(2), .bound = host(3), .schema = host(4)};
    m._words = cc::vector<u32>::create_uninitialized(count - k_header_words);
    for (auto i = k_header_words; i < count; ++i)
        m._words[i - k_header_words] = host(i);
    for (auto at = isize(0); at < m._words.size();)
    {
        auto const length = isize(m._words[at] >> 16);
        if (length == 0)
            return cc::error(cc::format("spirv: the instruction at word {} counts zero words", at + k_header_words));
        if (at + length > m._words.size())
            return cc::error(cc::format("spirv: the instruction at word {} runs {} words past the end",
                                        at + k_header_words, at + length - m._words.size()));
        m._starts.push_back(u32(at));
        at += length;
    }
    return m;
}

cc::result<module> read(cc::read_stream& in)
{
    auto bytes = in.read_all();
    if (bytes.has_error())
        return cc::error(cc::format("spirv: reading the module failed: {}", bytes.error().to_string()));
    return read(cc::span<byte const>(bytes.value()));
}
} // namespace babel::spirv
