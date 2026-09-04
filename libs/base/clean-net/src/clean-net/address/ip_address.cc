#include "ip_address.hh"

#include <clean-core/common/asserts.hh>
#include <clean-core/string/char_predicates.hh>

namespace cnet
{
namespace
{
constexpr isize k_v6_groups = 8;

void append_decimal(cc::string& out, u32 v)
{
    char buf[10];
    isize n = 0;
    do
    {
        buf[n++] = char('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0)
        out.push_back(buf[--n]);
}

void append_hex_lower(cc::string& out, u16 v)
{
    if (v == 0)
    {
        out.push_back('0');
        return;
    }
    char buf[4];
    isize n = 0;
    while (v != 0)
    {
        buf[n++] = "0123456789abcdef"[v & 0xF];
        v = u16(v >> 4);
    }
    while (n > 0)
        out.push_back(buf[--n]);
}

/// One decimal octet, 0-255, with no leading zero.
/// The leading zero is refused rather than read: `010` is octal to some resolvers and decimal to others, and that
/// disagreement is how an address allowlist gets walked past.
cc::optional<u8> parse_octet(cc::string_view s)
{
    if (s.empty() || s.size() > 3)
        return {};
    if (s.size() > 1 && s[0] == '0')
        return {};

    u32 value = 0;
    for (auto const c : s)
    {
        if (!cc::is_digit(c))
            return {};
        value = value * 10 + u32(c - '0');
    }
    if (value > 255)
        return {};
    return u8(value);
}

/// A dotted quad into four octets.
bool parse_v4_octets(cc::string_view text, u8* out)
{
    isize begin = 0;
    for (isize i = 0; i < 4; ++i)
    {
        auto end = text.find('.', begin);
        auto const last = i == 3;
        if (last)
        {
            if (end >= 0)
                return false; // a fifth group
            end = text.size();
        }
        else if (end < 0)
        {
            return false; // too few groups
        }

        auto const octet = parse_octet(text.subview({.start = begin, .end = end}));
        if (!octet.has_value())
            return false;
        out[i] = octet.value();
        begin = end + 1;
    }
    return true;
}

cc::optional<u16> parse_hex_group(cc::string_view s)
{
    if (s.empty() || s.size() > 4)
        return {};
    u32 value = 0;
    for (auto const c : s)
    {
        if (!cc::is_hex_digit(c))
            return {};
        auto const digit = cc::is_digit(c) ? u32(c - '0') : u32(cc::to_lower(c) - 'a') + 10;
        value = value * 16 + digit;
    }
    return u16(value);
}

/// Parse a colon-separated run of hex groups, where the LAST one may be a dotted quad contributing two groups.
/// `text` must not be empty and must carry no empty group.
bool parse_group_run(cc::string_view text, u16* out, isize& out_count, bool allow_v4_tail)
{
    out_count = 0;
    isize begin = 0;
    while (true)
    {
        auto end = text.find(':', begin);
        auto const last = end < 0;
        if (last)
            end = text.size();

        auto const group = text.subview({.start = begin, .end = end});
        if (group.empty())
            return false;

        if (last && allow_v4_tail && group.contains('.'))
        {
            u8 quad[4] = {};
            if (!parse_v4_octets(group, quad))
                return false;
            if (out_count + 2 > k_v6_groups)
                return false;
            out[out_count++] = u16(u32(quad[0]) << 8 | quad[1]);
            out[out_count++] = u16(u32(quad[2]) << 8 | quad[3]);
        }
        else
        {
            auto const parsed = parse_hex_group(group);
            if (!parsed.has_value())
                return false;
            if (out_count + 1 > k_v6_groups)
                return false;
            out[out_count++] = parsed.value();
        }

        if (last)
            return true;
        begin = end + 1;
    }
}

cc::optional<ip_address> parse_v6(cc::string_view text, u32 scope_id)
{
    u16 groups[k_v6_groups] = {};

    auto const double_colon = text.find("::");
    if (double_colon >= 0)
    {
        // Exactly one `::` -- a second one would make the number of elided groups ambiguous.
        if (text.find("::", double_colon + 1) >= 0)
            return {};

        auto const left = text.subview({.offset = 0, .size = double_colon});
        auto const right = text.subview(double_colon + 2);

        u16 head[k_v6_groups] = {};
        u16 tail[k_v6_groups] = {};
        isize head_count = 0;
        isize tail_count = 0;

        if (!left.empty() && !parse_group_run(left, head, head_count, false))
            return {};
        if (!right.empty() && !parse_group_run(right, tail, tail_count, true))
            return {};

        // `::` stands for one or more zero groups, so a full eight either side of it is not an elision.
        if (head_count + tail_count >= k_v6_groups)
            return {};

        for (isize i = 0; i < head_count; ++i)
            groups[i] = head[i];
        for (isize i = 0; i < tail_count; ++i)
            groups[k_v6_groups - tail_count + i] = tail[i];
    }
    else
    {
        isize count = 0;
        if (!parse_group_run(text, groups, count, true))
            return {};
        if (count != k_v6_groups)
            return {};
    }

    u8 octets[16] = {};
    for (isize i = 0; i < k_v6_groups; ++i)
    {
        octets[i * 2] = u8(groups[i] >> 8);
        octets[i * 2 + 1] = u8(groups[i] & 0xFF);
    }
    return ip_address::from_v6(cc::span<u8 const>(octets, 16), scope_id);
}

/// The longest run of two or more zero groups, leftmost on a tie; `length` is 0 when there is none.
void longest_zero_run(u16 const* groups, isize& start, isize& length)
{
    start = -1;
    length = 0;

    isize current_start = -1;
    isize current_length = 0;
    for (isize i = 0; i < k_v6_groups; ++i)
    {
        if (groups[i] == 0)
        {
            if (current_length == 0)
                current_start = i;
            ++current_length;
            if (current_length > length)
            {
                length = current_length;
                start = current_start;
            }
        }
        else
        {
            current_length = 0;
        }
    }

    // A single zero group is written out, since `::` for one group saves nothing and costs a reader.
    if (length < 2)
    {
        start = -1;
        length = 0;
    }
}
} // namespace

ip_address ip_address::from_v4(cc::span<u8 const> octets)
{
    CC_ASSERT(octets.size() == 4, "an IPv4 address is four octets");
    auto a = ip_address();
    a._family = ip_family::v4;
    for (isize i = 0; i < 4; ++i)
        a._octets[i] = octets[i];
    return a;
}

ip_address ip_address::from_v6(cc::span<u8 const> octets, u32 scope_id)
{
    CC_ASSERT(octets.size() == 16, "an IPv6 address is sixteen octets");
    auto a = ip_address();
    a._family = ip_family::v6;
    a._scope_id = scope_id;
    for (isize i = 0; i < 16; ++i)
        a._octets[i] = octets[i];
    return a;
}

ip_address ip_address::any(ip_family family)
{
    auto a = ip_address();
    a._family = family;
    return a;
}

ip_address ip_address::loopback(ip_family family)
{
    auto a = ip_address();
    a._family = family;
    if (family == ip_family::v4)
    {
        a._octets[0] = 127;
        a._octets[3] = 1;
    }
    else if (family == ip_family::v6)
    {
        a._octets[15] = 1;
    }
    return a;
}

cc::optional<ip_address> ip_address::parse(cc::string_view text)
{
    if (text.empty())
        return {};

    // A scope suffix belongs to IPv6 only, and the interface may be named rather than numbered on some platforms --
    // only the numeric form is accepted here, because resolving a name needs the OS.
    auto address_text = text;
    u32 scope_id = 0;
    auto const percent = text.find('%');
    if (percent >= 0)
    {
        auto const scope_text = text.subview(percent + 1);
        if (scope_text.empty())
            return {};
        u64 value = 0;
        for (auto const c : scope_text)
        {
            if (!cc::is_digit(c))
                return {};
            value = value * 10 + u64(c - '0');
            if (value > 0xFFFFFFFF)
                return {};
        }
        scope_id = u32(value);
        address_text = text.subview({.offset = 0, .size = percent});
    }

    if (address_text.contains(':'))
        return parse_v6(address_text, scope_id);

    // A scope on an IPv4 address is meaningless rather than merely unused.
    if (percent >= 0)
        return {};

    u8 quad[4] = {};
    if (!parse_v4_octets(address_text, quad))
        return {};
    return ip_address::from_v4(cc::span<u8 const>(quad, 4));
}

cc::span<u8 const> ip_address::octets() const
{
    switch (_family)
    {
    case ip_family::v4:
        return cc::span<u8 const>(_octets, 4);
    case ip_family::v6:
        return cc::span<u8 const>(_octets, 16);
    case ip_family::none:
        return {};
    }
    return {};
}

bool ip_address::is_unspecified() const
{
    auto const o = octets();
    if (o.empty())
        return false;
    for (auto const b : o)
        if (b != 0)
            return false;
    return true;
}

bool ip_address::is_loopback() const
{
    if (_family == ip_family::v4)
        return _octets[0] == 127;
    if (_family != ip_family::v6)
        return false;

    for (isize i = 0; i < 15; ++i)
        if (_octets[i] != 0)
            return false;
    return _octets[15] == 1;
}

bool ip_address::is_multicast() const
{
    if (_family == ip_family::v4)
        return (_octets[0] & 0xF0) == 0xE0;
    if (_family == ip_family::v6)
        return _octets[0] == 0xFF;
    return false;
}

bool ip_address::is_link_local() const
{
    if (_family == ip_family::v4)
        return _octets[0] == 169 && _octets[1] == 254;
    if (_family == ip_family::v6)
        return _octets[0] == 0xFE && (_octets[1] & 0xC0) == 0x80;
    return false;
}

bool ip_address::is_v4_mapped() const
{
    if (_family != ip_family::v6)
        return false;
    for (isize i = 0; i < 10; ++i)
        if (_octets[i] != 0)
            return false;
    return _octets[10] == 0xFF && _octets[11] == 0xFF;
}

cc::string ip_address::to_string() const
{
    auto out = cc::string();

    if (_family == ip_family::none)
        return out;

    if (_family == ip_family::v4)
    {
        for (isize i = 0; i < 4; ++i)
        {
            if (i > 0)
                out.push_back('.');
            append_decimal(out, _octets[i]);
        }
        return out;
    }

    u16 groups[k_v6_groups] = {};
    for (isize i = 0; i < k_v6_groups; ++i)
        groups[i] = u16(u32(_octets[i * 2]) << 8 | _octets[i * 2 + 1]);

    if (is_v4_mapped())
    {
        out += "::ffff:";
        for (isize i = 12; i < 16; ++i)
        {
            if (i > 12)
                out.push_back('.');
            append_decimal(out, _octets[i]);
        }
    }
    else
    {
        isize zero_start = -1;
        isize zero_length = 0;
        longest_zero_run(groups, zero_start, zero_length);

        for (isize i = 0; i < k_v6_groups;)
        {
            if (i == zero_start)
            {
                out += "::";
                i += zero_length;
                continue;
            }
            if (i > 0 && i != zero_start + zero_length)
                out.push_back(':');
            append_hex_lower(out, groups[i]);
            ++i;
        }
    }

    if (_scope_id != 0)
    {
        out.push_back('%');
        append_decimal(out, _scope_id);
    }
    return out;
}
} // namespace cnet
