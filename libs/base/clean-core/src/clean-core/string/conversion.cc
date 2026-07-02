#include <clean-core/string/conversion.hh>

cc::vector<char16_t> cc::utf8_to_utf16(cc::string_view utf8)
{
    cc::vector<char16_t> out;
    out.reserve_back(utf8.size());

    auto const* bytes = reinterpret_cast<unsigned char const*>(utf8.data());
    isize const count = utf8.size();

    auto const emit = [&](u32 cp)
    {
        if (cp <= 0xFFFF)
            out.push_back(char16_t(cp));
        else // astral: encode as a surrogate pair
        {
            cp -= 0x10000;
            out.push_back(char16_t(0xD800 + (cp >> 10)));
            out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        }
    };
    auto const is_continuation = [&](isize i) { return i < count && (bytes[i] & 0xC0) == 0x80; };

    isize i = 0;
    while (i < count)
    {
        u32 const lead = bytes[i];

        if (lead < 0x80) // 0xxxxxxx: ASCII
        {
            emit(lead);
            i += 1;
        }
        else if ((lead >> 5) == 0x6 && is_continuation(i + 1)) // 110xxxxx: 2 bytes
        {
            u32 const cp = ((lead & 0x1F) << 6) | (bytes[i + 1] & 0x3F);
            emit(cp < 0x80 ? 0xFFFD : cp); // reject overlong
            i += 2;
        }
        else if ((lead >> 4) == 0xE && is_continuation(i + 1) && is_continuation(i + 2)) // 1110xxxx: 3 bytes
        {
            u32 const cp = ((lead & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
            emit(cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF) ? 0xFFFD : cp); // reject overlong / surrogates
            i += 3;
        }
        else if ((lead >> 3) == 0x1E && is_continuation(i + 1) && is_continuation(i + 2)
                 && is_continuation(i + 3)) // 11110xxx: 4 bytes
        {
            u32 const cp = ((lead & 0x07) << 18) | ((bytes[i + 1] & 0x3F) << 12) | ((bytes[i + 2] & 0x3F) << 6)
                         | (bytes[i + 3] & 0x3F);
            emit(cp < 0x10000 || cp > 0x10FFFF ? 0xFFFD : cp); // reject overlong / out of range
            i += 4;
        }
        else // invalid lead byte or truncated sequence
        {
            emit(0xFFFD);
            i += 1;
        }
    }

    return out;
}
