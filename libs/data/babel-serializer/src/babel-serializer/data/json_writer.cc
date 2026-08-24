#include <babel-serializer/data/json.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::unit, cc::min
#include <clean-core/string/to_string.hh>

using namespace babel;

namespace
{
// 32 is enough for one memcpy per level at any sane indent, and the loop covers the rest.
constexpr char k_spaces[32] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                               ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

constexpr char k_hex_digits[] = "0123456789abcdef";

/// Finite means the exponent bits are not all ones, which is exact and needs no <cmath>.
bool is_finite(double v)
{
    u64 bits = 0;
    cc::memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

/// Decodes one UTF-8 code point, or reports the byte as un-decodable.
/// Lenient by design: a malformed sequence yields `ok == false` and a length of 1, so the caller can pass the byte on
/// untouched rather than rewriting the input.
struct decoded
{
    u32 code_point = 0;
    i32 length = 1;
    bool ok = false;
};

decoded decode_utf8(cc::string_view s, isize i)
{
    auto const available = s.size() - i;
    auto const b0 = u8(s[i]);

    auto const continuation = [&](isize k) { return k < available && (u8(s[i + k]) & 0xC0) == 0x80; };

    if (b0 < 0x80)
        return {.code_point = b0, .length = 1, .ok = true};

    if ((b0 & 0xE0) == 0xC0 && continuation(1))
        return {.code_point = (u32(b0 & 0x1F) << 6) | u32(u8(s[i + 1]) & 0x3F), .length = 2, .ok = true};

    if ((b0 & 0xF0) == 0xE0 && continuation(1) && continuation(2))
        return {.code_point = (u32(b0 & 0x0F) << 12) | (u32(u8(s[i + 1]) & 0x3F) << 6) | u32(u8(s[i + 2]) & 0x3F),
                .length = 3,
                .ok = true};

    if ((b0 & 0xF8) == 0xF0 && continuation(1) && continuation(2) && continuation(3))
        return {.code_point = (u32(b0 & 0x07) << 18) | (u32(u8(s[i + 1]) & 0x3F) << 12)
                            | (u32(u8(s[i + 2]) & 0x3F) << 6) | u32(u8(s[i + 3]) & 0x3F),
                .length = 4,
                .ok = true};

    return {};
}
} // namespace

// -------------------------------------------------------------------------------------------------
// the low-level emitters

void json::writer::impl_emit(cc::string_view text)
{
    if (!_error.is_empty()) // sticky: every write after the first failure is a no-op
        return;

    auto r = _out->write(cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size()));
    if (!r.has_value())
        _error = cc::move(r).error();
}

void json::writer::impl_fail(cc::string message)
{
    if (_error.is_empty())
        _error = cc::any_error(cc::move(message));
}

void json::writer::impl_indent()
{
    if (_opts.indent <= 0 || _opts.newline_delimited)
        return;

    this->impl_emit("\n");
    auto remaining = _opts.indent * i32(_stack.size());
    while (remaining > 0)
    {
        auto const n = cc::min(remaining, i32(sizeof(k_spaces)));
        this->impl_emit(cc::string_view(k_spaces, n));
        remaining -= n;
    }
}

// -------------------------------------------------------------------------------------------------
// value separators and the scope stack

void json::writer::impl_begin_root()
{
    CC_ASSERT(_stack.empty(), "a root value is only written outside every scope");
    CC_ASSERT(!_finished, "the writer is already finished");
    CC_ASSERT(_root_count == 0 || _opts.newline_delimited, "a JSON document holds one root value; set "
                                                           "write_options::newline_delimited for several");

    if (_root_count > 0)
        this->impl_emit("\n");
    ++_root_count;
}

void json::writer::impl_begin_member(cc::string_view key)
{
    CC_ASSERT(!_stack.empty() && _stack.back().is_object, "a keyed write needs an open object scope");

    auto& lvl = _stack.back();
    if (lvl.has_any)
        this->impl_emit(",");
    lvl.has_any = true;

    this->impl_indent();
    this->impl_string(key, _opts.escape_non_ascii);
    this->impl_emit(_opts.indent > 0 && !_opts.newline_delimited ? ": " : ":");
}

void json::writer::impl_begin_element()
{
    CC_ASSERT(!_stack.empty() && !_stack.back().is_object, "an unkeyed write needs an open array scope");

    auto& lvl = _stack.back();
    if (lvl.has_any)
        this->impl_emit(",");
    lvl.has_any = true;

    this->impl_indent();
}

void json::writer::impl_open(bool is_object)
{
    this->impl_emit(is_object ? "{" : "[");
    _stack.push_back({.is_object = is_object, .has_any = false});
}

void json::writer::impl_close(i32 depth)
{
    if (i32(_stack.size()) < depth) // already closed by finish()
        return;
    CC_ASSERT(i32(_stack.size()) == depth, "JSON scopes must be closed innermost-first");

    auto const lvl = _stack.back();
    _stack.pop_back();
    if (lvl.has_any)
        this->impl_indent(); // the closing bracket lines up with what opened it, one level out
    this->impl_emit(lvl.is_object ? "}" : "]");
}

// -------------------------------------------------------------------------------------------------
// the values themselves

void json::writer::impl_null()
{
    this->impl_emit("null");
}

void json::writer::impl_bool(bool v)
{
    this->impl_emit(v ? "true" : "false");
}

void json::writer::impl_i64(i64 v)
{
    char buf[cc::to_chars_int_max];
    this->impl_emit(cc::string_view(buf, cc::to_chars(buf, (long long)(v))));
}

void json::writer::impl_u64(u64 v)
{
    char buf[cc::to_chars_int_max];
    this->impl_emit(cc::string_view(buf, cc::to_chars(buf, (unsigned long long)(v))));
}

void json::writer::impl_double(double v, cc::float_notation notation, i32 precision)
{
    if (!is_finite(v))
    {
        switch (_opts.non_finite)
        {
        case non_finite_policy::error:
            this->impl_fail("json writer: NaN and infinity are not representable in JSON");
            return;
        case non_finite_policy::null:
            this->impl_emit("null");
            return;
        case non_finite_policy::string:
            this->impl_emit(v != v ? "\"NaN\"" : v > 0 ? "\"Infinity\"" : "\"-Infinity\"");
            return;
        }
    }

    char buf[cc::to_chars_float_max];
    this->impl_emit(cc::string_view(buf, cc::to_chars(buf, v, notation, precision)));
}

void json::writer::impl_float(float v, cc::float_notation notation, i32 precision)
{
    if (!is_finite(double(v)))
    {
        this->impl_double(double(v), notation, precision); // one policy, one place
        return;
    }

    char buf[cc::to_chars_float_max];
    this->impl_emit(cc::string_view(buf, cc::to_chars(buf, v, notation, precision)));
}

void json::writer::impl_raw(cc::string_view fragment)
{
    this->impl_emit(fragment);
}

void json::writer::impl_string(cc::string_view v, bool escape_non_ascii)
{
    this->impl_emit("\"");

    // clean runs are emitted whole; only the bytes that must change are handled one at a time
    auto run_start = isize(0);
    auto const flush_run = [&](isize end)
    {
        if (end > run_start)
            this->impl_emit(v.subview({.offset = run_start, .size = end - run_start}));
    };

    auto const emit_unicode_escape = [&](u32 code_unit)
    {
        char esc[6] = {'\\', 'u', 0, 0, 0, 0};
        esc[2] = k_hex_digits[(code_unit >> 12) & 0xF];
        esc[3] = k_hex_digits[(code_unit >> 8) & 0xF];
        esc[4] = k_hex_digits[(code_unit >> 4) & 0xF];
        esc[5] = k_hex_digits[code_unit & 0xF];
        this->impl_emit(cc::string_view(esc, 6));
    };

    for (auto i = isize(0); i < v.size();)
    {
        auto const c = u8(v[i]);

        if (c >= 0x80)
        {
            if (!escape_non_ascii) // pass the UTF-8 through untouched, which is the default
            {
                ++i;
                continue;
            }

            auto const d = decode_utf8(v, i);
            if (!d.ok) // undecodable byte: leave it exactly as it came in
            {
                ++i;
                continue;
            }

            flush_run(i);
            if (d.code_point <= 0xFFFF)
                emit_unicode_escape(d.code_point);
            else // astral: JSON has no \U, so it goes out as the surrogate pair
            {
                auto const rest = d.code_point - 0x10000;
                emit_unicode_escape(0xD800 + (rest >> 10));
                emit_unicode_escape(0xDC00 + (rest & 0x3FF));
            }
            i += d.length;
            run_start = i;
            continue;
        }

        cc::string_view escape;
        switch (c)
        {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }

        if (!escape.empty())
        {
            flush_run(i);
            this->impl_emit(escape);
            ++i;
            run_start = i;
            continue;
        }

        if (c < 0x20) // every other control character has no short form
        {
            flush_run(i);
            emit_unicode_escape(c);
            ++i;
            run_start = i;
            continue;
        }

        ++i;
    }

    flush_run(v.size());
    this->impl_emit("\"");
}

// -------------------------------------------------------------------------------------------------
// scopes and finishing

json::object_writer json::writer::object()
{
    this->impl_begin_root();
    this->impl_open(true);
    return object_writer(this, i32(_stack.size()));
}

json::array_writer json::writer::array()
{
    this->impl_begin_root();
    this->impl_open(false);
    return array_writer(this, i32(_stack.size()));
}

json::object_writer json::object_writer::write_object(cc::string_view key)
{
    CC_ASSERT(_w->impl_depth() == _depth, "a scope with an open child cannot be written to");
    _w->impl_begin_member(key);
    _w->impl_open(true);
    return object_writer(_w, _depth + 1);
}

json::array_writer json::object_writer::write_array(cc::string_view key)
{
    CC_ASSERT(_w->impl_depth() == _depth, "a scope with an open child cannot be written to");
    _w->impl_begin_member(key);
    _w->impl_open(false);
    return array_writer(_w, _depth + 1);
}

json::object_writer json::array_writer::write_object()
{
    CC_ASSERT(_w->impl_depth() == _depth, "a scope with an open child cannot be written to");
    _w->impl_begin_element();
    _w->impl_open(true);
    return object_writer(_w, _depth + 1);
}

json::array_writer json::array_writer::write_array()
{
    CC_ASSERT(_w->impl_depth() == _depth, "a scope with an open child cannot be written to");
    _w->impl_begin_element();
    _w->impl_open(false);
    return array_writer(_w, _depth + 1);
}

json::object_writer::~object_writer()
{
    if (_w != nullptr)
        _w->impl_close(_depth);
}

json::array_writer::~array_writer()
{
    if (_w != nullptr)
        _w->impl_close(_depth);
}

cc::result<cc::unit> json::writer::finish()
{
    if (!_finished)
    {
        while (!_stack.empty())
            this->impl_close(i32(_stack.size()));
        if (_opts.newline_delimited && _root_count > 0)
            this->impl_emit("\n"); // every record on its own line, the last one included

        _finished = true;

        if (_error.is_empty())
        {
            auto r = _out->flush();
            if (!r.has_value())
                _error = cc::move(r).error();
        }
    }

    if (!_error.is_empty())
        return cc::error(cc::move(_error));
    return cc::unit{};
}

json::writer::~writer()
{
    if (!_finished)
        (void)this->finish(); // best-effort: an early return must not assert, and there is nobody to report to
}

cc::result<cc::string> json::string_writer::finish()
{
    CC_RETURN_IF_ERROR(_writer.finish());
    return _buffer.take();
}
