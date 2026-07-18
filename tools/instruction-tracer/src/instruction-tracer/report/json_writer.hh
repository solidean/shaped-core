#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace itrace
{
using namespace cc::primitive_defines;

/// A minimal JSON emitter, just enough to serialize the trace model for the HTML export. Not a
/// general library — no pretty-printing, no validation beyond nesting bookkeeping.
///
/// Two rules the export depends on:
///   * u64 values go out as JSON *strings* — JS numbers are IEEE doubles and lose precision past
///     2^53, which every address exceeds. Emit addresses/register values with value_string.
///   * Every string escapes '<' as <, so a symbol or source line containing "</script>" cannot
///     break out of the <script> tag it is embedded in. The JS parser turns < back into '<',
///     so the data is unchanged — only the byte sequence in the file is made safe.
class json_writer
{
public:
    void begin_object()
    {
        sep();
        _out += '{';
        _first.push_back(true);
    }
    void end_object()
    {
        _out += '}';
        _first.remove_back();
    }
    void begin_array()
    {
        sep();
        _out += '[';
        _first.push_back(true);
    }
    void end_array()
    {
        _out += ']';
        _first.remove_back();
    }

    /// Emit a key. The next value belongs to it (no separator before that value).
    void key(cc::string_view k)
    {
        sep();
        emit_string(k);
        _out += ':';
        _after_key = true;
    }

    void value_string(cc::string_view s)
    {
        sep();
        emit_string(s);
    }
    void value_bool(bool b)
    {
        sep();
        _out += b ? "true" : "false";
    }
    void value_null()
    {
        sep();
        _out += "null";
    }
    void value_int(i64 n)
    {
        sep();
        _out += cc::format("{}", n);
    }
    void value_uint(u64 n)
    {
        sep();
        _out += cc::format("{}", n);
    }

    // key + value convenience.
    void field(cc::string_view k, cc::string_view s)
    {
        key(k);
        value_string(s);
    }
    void field_bool(cc::string_view k, bool b)
    {
        key(k);
        value_bool(b);
    }
    void field_int(cc::string_view k, i64 n)
    {
        key(k);
        value_int(n);
    }
    void field_uint(cc::string_view k, u64 n)
    {
        key(k);
        value_uint(n);
    }

    [[nodiscard]] cc::string const& str() const { return _out; }

private:
    /// Emit the comma between siblings. A value right after a key is not a sibling, so it is skipped.
    void sep()
    {
        if (_after_key)
        {
            _after_key = false;
            return;
        }
        if (!_first.empty())
        {
            if (!_first.back())
                _out += ',';
            _first.back() = false;
        }
    }

    void emit_string(cc::string_view s)
    {
        _out += '"';
        for (char const c : s)
        {
            switch (c)
            {
            case '"':
                _out += "\\\"";
                break;
            case '\\':
                _out += "\\\\";
                break;
            case '\n':
                _out += "\\n";
                break;
            case '\r':
                _out += "\\r";
                break;
            case '\t':
                _out += "\\t";
                break;
            case '\b':
                _out += "\\b";
                break;
            case '\f':
                _out += "\\f";
                break;
            case '<': // break "</script>" and "<!--" without changing the parsed value
                _out += "\\u003c";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    _out += cc::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                else
                    _out += c;
                break;
            }
        }
        _out += '"';
    }

    cc::string _out;
    cc::vector<bool> _first; // per nesting level: is the next sibling the first?
    bool _after_key = false;
};
} // namespace itrace
