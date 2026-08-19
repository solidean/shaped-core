#include <babel-serializer/data/base64.hh>
#include <clean-core/common/utility.hh> // cc::move, cc::unit
#include <clean-core/string/format.hh>

// One 256-entry table drives the whole decoder: an entry of 0..63 is a symbol value, and the three negative
// sentinels distinguish padding, skippable whitespace and reject — so the walk needs no character classification.
// Both alphabets share the table, which is why a mixed-alphabet input decodes without a mode flag.

namespace babel::base64
{
namespace
{
constexpr i8 sym_reject = -1;
constexpr i8 sym_padding = -2;
constexpr i8 sym_space = -3;

// A plain array in a struct, not cc::array: this must be a compile-time constant, and cc::array allocates.
struct symbol_table
{
    i8 values[256] = {};
};

constexpr symbol_table make_symbol_table()
{
    auto table = symbol_table();
    for (auto& v : table.values)
        v = sym_reject;

    for (auto c = 0; c < 26; ++c)
        table.values['A' + c] = i8(c);
    for (auto c = 0; c < 26; ++c)
        table.values['a' + c] = i8(26 + c);
    for (auto c = 0; c < 10; ++c)
        table.values['0' + c] = i8(52 + c);

    table.values['+'] = i8(62); // standard alphabet
    table.values['/'] = i8(63);
    table.values['-'] = i8(62); // URL-safe alphabet
    table.values['_'] = i8(63);

    table.values['='] = sym_padding;

    for (auto const c : {' ', '\t', '\n', '\r', '\v', '\f'})
        table.values[c] = sym_space;

    return table;
}

constexpr auto symbols_of = make_symbol_table();

constexpr char encode_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Validate `text` and return the number of bytes it decodes to, writing them into `out` when `write` is set.
/// With `write` false `out` is ignored entirely, which is what makes this both the validator and the decoder.
cc::result<isize> decode_core(cc::string_view text, cc::span<byte> out, bool write)
{
    auto written = isize(0);
    auto quantum = u32(0); // the accumulated symbols, 6 bits each, low-aligned
    auto symbols = 0;      // how many symbols `quantum` holds, 0..3
    auto padding = 0;      // '=' characters seen since the last complete quantum

    // Emit the top `count` bytes of a 24-bit-aligned `quantum`.
    auto const emit = [&](int count) -> cc::result<cc::unit>
    {
        if (write && written + count > out.size())
            return cc::error(cc::format("base64: output buffer holds only {} bytes", out.size()));

        for (auto k = 0; k < count; ++k)
        {
            if (write)
                out[written] = byte(u8((quantum >> (16 - 8 * k)) & 0xFFu));
            ++written;
        }
        return cc::unit{};
    };

    for (auto i = isize(0); i < text.size(); ++i)
    {
        auto const c = text[i];
        auto const v = symbols_of.values[u8(c)];

        if (v == sym_space)
            continue;

        if (v == sym_padding)
        {
            // Padding closes the final quantum, so it is only meaningful once at least one symbol is pending.
            if (symbols == 0 && padding == 0)
                return cc::error(cc::format("base64: padding '=' at offset {} does not close a quantum", i));
            if (symbols + padding >= 4)
                return cc::error(cc::format("base64: excess padding '=' at offset {}", i));
            ++padding;
            continue;
        }

        if (v == sym_reject)
            return cc::error(cc::format("base64: invalid character 0x{:02x} at offset {}", u32(u8(c)), i));

        if (padding > 0)
            return cc::error(cc::format("base64: data character at offset {} follows padding", i));

        quantum = (quantum << 6) | u32(v);
        ++symbols;

        if (symbols == 4)
        {
            CC_RETURN_IF_ERROR(emit(3));
            quantum = 0;
            symbols = 0;
        }
    }

    // A trailing partial quantum encodes 1 byte from 2 symbols or 2 bytes from 3; a lone symbol encodes nothing.
    switch (symbols)
    {
    case 0:
        break;
    case 1:
        return cc::error("base64: truncated final quantum (a single trailing character encodes no byte)");
    case 2:
        quantum <<= 12;
        CC_RETURN_IF_ERROR(emit(1));
        break;
    default:
        quantum <<= 6;
        CC_RETURN_IF_ERROR(emit(2));
        break;
    }

    return written;
}
} // namespace

cc::optional<isize> decoded_size(cc::string_view text)
{
    auto const size = decode_core(text, {}, false);
    if (size.has_error())
        return cc::nullopt;
    return size.value();
}

cc::result<cc::vector<byte>> decode(cc::string_view text)
{
    // Upper bound: 4 characters yield at most 3 bytes, and whitespace / padding only ever shrink that.
    auto bytes = cc::vector<byte>();
    bytes.resize_to_uninitialized(text.size() / 4 * 3 + 3);

    auto written = decode_core(text, bytes, true);
    CC_RETURN_IF_ERROR(written);

    bytes.resize_down_to(written.value());
    return cc::move(bytes);
}

cc::result<isize> decode_into(cc::string_view text, cc::span<byte> out)
{
    return decode_core(text, out, true);
}

cc::string encode(cc::span<byte const> bytes)
{
    auto text = cc::string();
    text.reserve_back((bytes.size() + 2) / 3 * 4);

    for (auto i = isize(0); i < bytes.size(); i += 3)
    {
        auto const remaining = bytes.size() - i;
        auto const b0 = u32(u8(bytes[i]));
        auto const b1 = remaining > 1 ? u32(u8(bytes[i + 1])) : 0u;
        auto const b2 = remaining > 2 ? u32(u8(bytes[i + 2])) : 0u;
        auto const quantum = (b0 << 16) | (b1 << 8) | b2;

        text.push_back(encode_alphabet[(quantum >> 18) & 0x3Fu]);
        text.push_back(encode_alphabet[(quantum >> 12) & 0x3Fu]);
        text.push_back(remaining > 1 ? encode_alphabet[(quantum >> 6) & 0x3Fu] : '=');
        text.push_back(remaining > 2 ? encode_alphabet[quantum & 0x3Fu] : '=');
    }

    return text;
}
} // namespace babel::base64
