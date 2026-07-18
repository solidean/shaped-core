#pragma once

#include <clean-core/container/pair.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

#include <charconv>

namespace itrace
{
using namespace cc::primitive_defines;

/// A minimal recursive-descent JSON reader, scoped to what `llvm-mca -json` emits: objects, arrays,
/// strings (with `\uXXXX` escapes), numbers (int and float), bool, null. Not spec-complete and not a
/// general library — lookup is first-key-wins, big integers lose precision past a double's 2^53.
///
/// TODO: replace with a clean-core JSON reader once one exists. clean-core has only a JSON *writer*
/// today (report/json_writer.hh); this fills the reader gap locally until that lands, at which point
/// json_value / parse_json here should be dropped in favor of the shared one.
struct json_value
{
    enum class kind
    {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    kind k = kind::null;
    bool b = false;
    double num = 0;                                   // int and float both land here
    cc::string str;                                   // unescaped
    cc::vector<json_value> arr;                       // when k == array
    cc::vector<cc::pair<cc::string, json_value>> obj; // when k == object, order-preserving

    [[nodiscard]] bool is_null() const { return k == kind::null; }
    [[nodiscard]] bool is_bool() const { return k == kind::boolean; }
    [[nodiscard]] bool is_number() const { return k == kind::number; }
    [[nodiscard]] bool is_string() const { return k == kind::string; }
    [[nodiscard]] bool is_array() const { return k == kind::array; }
    [[nodiscard]] bool is_object() const { return k == kind::object; }

    /// Kind-tolerant accessors: return the fallback when the value is not of the asked type.
    [[nodiscard]] double as_number(double fallback = 0) const { return k == kind::number ? num : fallback; }
    [[nodiscard]] bool as_bool(bool fallback = false) const { return k == kind::boolean ? b : fallback; }
    [[nodiscard]] cc::string_view as_string(cc::string_view fallback = {}) const
    {
        return k == kind::string ? cc::string_view(str) : fallback;
    }

    /// Object member by key, or nullptr when not an object / key absent (first match wins).
    [[nodiscard]] json_value const* find(cc::string_view key) const
    {
        if (k != kind::object)
            return nullptr;
        for (auto const& kv : obj)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }

    /// Element count for arrays and objects; 0 otherwise.
    [[nodiscard]] isize size() const
    {
        if (k == kind::array)
            return arr.size();
        if (k == kind::object)
            return obj.size();
        return 0;
    }

    /// Array element by index, or nullptr when out of range / not an array.
    [[nodiscard]] json_value const* at(isize i) const
    {
        return (k == kind::array && i >= 0 && i < arr.size()) ? &arr[i] : nullptr;
    }
};

namespace impl
{
/// Single-pass cursor with sticky error state: on the first malformed token a parser records the
/// message and every later parse_* short-circuits to a default. parse_json converts that to a result.
struct json_parser
{
    char const* begin = nullptr;
    char const* p = nullptr;
    char const* end = nullptr;
    bool failed = false;
    cc::string error_msg;

    [[nodiscard]] isize offset() const { return isize(p - begin); }

    void fail(cc::string_view what)
    {
        if (!failed)
        {
            failed = true;
            error_msg = cc::format("JSON parse error at offset {}: {}", offset(), what);
        }
    }

    void skip_ws()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    static void encode_utf8(u32 cp, cc::string& out)
    {
        if (cp <= 0x7f)
            out += char(cp);
        else if (cp <= 0x7ff)
        {
            out += char(0xc0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3f));
        }
        else if (cp <= 0xffff)
        {
            out += char(0xe0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
        else
        {
            out += char(0xf0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3f));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
    }

    u32 parse_hex4()
    {
        if (end - p < 4)
        {
            fail("truncated \\u escape");
            return 0;
        }
        u32 v = 0;
        for (int i = 0; i < 4; ++i)
        {
            char const c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= u32(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= u32(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= u32(c - 'A' + 10);
            else
            {
                fail("invalid hex digit in \\u escape");
                return 0;
            }
        }
        return v;
    }

    // p is at the opening quote.
    cc::string parse_string_raw()
    {
        ++p; // consume opening quote
        cc::string out;
        while (p < end)
        {
            char const c = *p++;
            if (c == '"')
                return out;
            if (c == '\\')
            {
                if (p >= end)
                    break;
                char const e = *p++;
                switch (e)
                {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u':
                {
                    u32 const hi = parse_hex4();
                    if (failed)
                        return {};
                    u32 cp = hi;
                    if (hi >= 0xd800 && hi <= 0xdbff && end - p >= 2 && p[0] == '\\' && p[1] == 'u')
                    {
                        p += 2;
                        u32 const lo = parse_hex4();
                        if (failed)
                            return {};
                        cp = 0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00);
                    }
                    encode_utf8(cp, out);
                    break;
                }
                default:
                    fail("invalid escape sequence");
                    return {};
                }
            }
            else
                out += c;
        }
        fail("unterminated string");
        return {};
    }

    json_value parse_number()
    {
        char const* const start = p;
        if (p < end && (*p == '-' || *p == '+'))
            ++p;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
            ++p;

        json_value v;
        v.k = json_value::kind::number;
        auto const [ptr, ec] = std::from_chars(start, p, v.num);
        if (ec != std::errc() || ptr != p)
        {
            p = start;
            fail("invalid number");
            return {};
        }
        return v;
    }

    json_value parse_object()
    {
        ++p; // consume '{'
        json_value v;
        v.k = json_value::kind::object;
        skip_ws();
        if (p < end && *p == '}')
        {
            ++p;
            return v;
        }
        while (true)
        {
            skip_ws();
            if (p >= end || *p != '"')
            {
                fail("expected string key in object");
                return {};
            }
            cc::string key = parse_string_raw();
            if (failed)
                return {};
            skip_ws();
            if (p >= end || *p != ':')
            {
                fail("expected ':' after object key");
                return {};
            }
            ++p;
            json_value val = parse_value();
            if (failed)
                return {};
            v.obj.push_back({cc::move(key), cc::move(val)});
            skip_ws();
            if (p >= end)
            {
                fail("unterminated object");
                return {};
            }
            if (*p == ',')
            {
                ++p;
                continue;
            }
            if (*p == '}')
            {
                ++p;
                return v;
            }
            fail("expected ',' or '}' in object");
            return {};
        }
    }

    json_value parse_array()
    {
        ++p; // consume '['
        json_value v;
        v.k = json_value::kind::array;
        skip_ws();
        if (p < end && *p == ']')
        {
            ++p;
            return v;
        }
        while (true)
        {
            json_value elem = parse_value();
            if (failed)
                return {};
            v.arr.push_back(cc::move(elem));
            skip_ws();
            if (p >= end)
            {
                fail("unterminated array");
                return {};
            }
            if (*p == ',')
            {
                ++p;
                continue;
            }
            if (*p == ']')
            {
                ++p;
                return v;
            }
            fail("expected ',' or ']' in array");
            return {};
        }
    }

    json_value parse_literal(cc::string_view word, json_value value)
    {
        if (end - p < word.size() || cc::string_view(p, word.size()) != word)
        {
            fail("invalid literal");
            return {};
        }
        p += word.size();
        return value;
    }

    json_value parse_value()
    {
        skip_ws();
        if (p >= end)
        {
            fail("unexpected end of input");
            return {};
        }

        switch (*p)
        {
        case '{':
            return parse_object();
        case '[':
            return parse_array();
        case '"':
        {
            json_value v;
            v.k = json_value::kind::string;
            v.str = parse_string_raw();
            return v;
        }
        case 't':
        {
            json_value v;
            v.k = json_value::kind::boolean;
            v.b = true;
            return parse_literal("true", cc::move(v));
        }
        case 'f':
        {
            json_value v;
            v.k = json_value::kind::boolean;
            v.b = false;
            return parse_literal("false", cc::move(v));
        }
        case 'n':
            return parse_literal("null", json_value{});
        default:
            return parse_number();
        }
    }
};
} // namespace impl

/// Parse a complete JSON document. Trailing whitespace is allowed; trailing junk is an error.
inline cc::result<json_value> parse_json(cc::string_view text)
{
    impl::json_parser parser;
    parser.begin = text.data();
    parser.p = text.data();
    parser.end = text.data() + text.size();

    json_value v = parser.parse_value();
    if (parser.failed)
        return cc::error(cc::move(parser.error_msg));

    parser.skip_ws();
    if (parser.p != parser.end)
        return cc::error(
            cc::format("JSON parse error at offset {}: trailing characters after top-level value", parser.offset()));

    return v;
}
} // namespace itrace
