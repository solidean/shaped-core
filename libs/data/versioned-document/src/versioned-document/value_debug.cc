#include "value_debug.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr u64 quiet_nan_bits = 0x7FF8000000000000ull;
constexpr u64 negative_zero_bits = 0x8000000000000000ull;
constexpr u64 exponent_mask = 0x7FF0000000000000ull;
constexpr u64 mantissa_mask = 0x000FFFFFFFFFFFFFull;

void append_hex_byte(cc::string& out, u8 v)
{
    constexpr char digits[] = "0123456789abcdef";
    out.push_back(digits[v >> 4]);
    out.push_back(digits[v & 0xF]);
}

void append_number(cc::string& out, f64 v)
{
    auto const bits = cc::bit_cast<u64>(v);

    if ((bits & exponent_mask) == exponent_mask)
    {
        if ((bits & mantissa_mask) == 0)
        {
            out += (bits & negative_zero_bits) != 0 ? "-Infinity" : "Infinity";
            return;
        }

        // the payload is shown whenever it is not the canonical quiet NaN, because two NaNs that differ here are
        // two different values as far as this codec is concerned
        if (bits == quiet_nan_bits)
            out += "NaN";
        else
            cc::format_append(out, "NaN(0x{:016x})", bits);

        return;
    }

    if (bits == negative_zero_bits)
    {
        out += "-0.0";
        return;
    }

    cc::format_append(out, "{}", v);
}

void append_string(cc::string& out, cc::string_view v)
{
    out.push_back('"');
    for (isize i = 0; i < v.size(); ++i)
    {
        auto const c = u8(v[i]);
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(char(c));
        }
        else if (c < 0x20 || c >= 0x7F)
        {
            out += "\\x";
            append_hex_byte(out, c);
        }
        else
        {
            out.push_back(char(c));
        }
    }
    out.push_back('"');
}

void append_value(cc::string& out, vdoc::value_view v)
{
    using vdoc::value_kind;

    switch (v.kind())
    {
    case value_kind::null:
        out += "null";
        return;

    case value_kind::boolean:
        out += v.as_bool() ? "true" : "false";
        return;

    case value_kind::integer:
        cc::format_append(out, "{}", v.as_i64());
        return;

    case value_kind::number:
        append_number(out, v.as_f64());
        return;

    case value_kind::string:
        append_string(out, v.as_string());
        return;

    case value_kind::bytes:
    {
        auto const data = v.as_bytes();
        out += "bytes(";
        for (isize i = 0; i < data.size(); ++i)
            append_hex_byte(out, u8(data[i]));
        out.push_back(')');
        return;
    }

    case value_kind::array:
    {
        out.push_back('[');
        for (isize i = 0; i < v.size(); ++i)
        {
            if (i > 0)
                out += ", ";
            append_value(out, v.element_at(i));
        }
        out.push_back(']');
        return;
    }

    case value_kind::object:
    {
        out.push_back('{');
        for (isize i = 0; i < v.size(); ++i)
        {
            if (i > 0)
                out += ", ";
            append_string(out, v.key_at(i));
            out += ": ";
            append_value(out, v.element_at(i));
        }
        out.push_back('}');
        return;
    }
    }

    CC_UNREACHABLE("tag out of range on bytes that passed try_decode");
}
} // namespace

cc::string vdoc::to_debug_string(value_view v)
{
    auto out = cc::string();
    append_value(out, v);
    return out;
}
