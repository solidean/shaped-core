#include <babel-serializer/data/json.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::unit, cc::min
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/to_string.hh>

using namespace babel;

namespace
{
// 32 is enough for one memcpy per level at any sane indent, and the loop covers the rest.
constexpr char k_spaces[32] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                               ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

constexpr char k_hex_digits[] = "0123456789abcdef";

/// The largest integer a double holds exactly: 2^53.
/// 2^53 itself round-trips; 2^53 + 1 does not, which is where a JSON number stops being able to carry an id.
constexpr u64 k_exact_integer_max = u64(1) << 53;

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

    // A well-formed sequence is not just one whose bits parse: an overlong encoding, a surrogate that came in as
    // UTF-8, and anything past U+10FFFF all decode "successfully" into something no escape can represent.
    // Reporting them as undecodable is what keeps them on the passthrough path, counted rather than rewritten.
    if ((b0 & 0xE0) == 0xC0 && continuation(1))
    {
        auto const cp = (u32(b0 & 0x1F) << 6) | u32(u8(s[i + 1]) & 0x3F);
        if (cp >= 0x80)
            return {.code_point = cp, .length = 2, .ok = true};
    }
    else if ((b0 & 0xF0) == 0xE0 && continuation(1) && continuation(2))
    {
        auto const cp = (u32(b0 & 0x0F) << 12) | (u32(u8(s[i + 1]) & 0x3F) << 6) | u32(u8(s[i + 2]) & 0x3F);
        if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF))
            return {.code_point = cp, .length = 3, .ok = true};
    }
    else if ((b0 & 0xF8) == 0xF0 && continuation(1) && continuation(2) && continuation(3))
    {
        auto const cp = (u32(b0 & 0x07) << 18) | (u32(u8(s[i + 1]) & 0x3F) << 12) | (u32(u8(s[i + 2]) & 0x3F) << 6)
                      | u32(u8(s[i + 3]) & 0x3F);
        if (cp >= 0x10000 && cp <= 0x10FFFF)
            return {.code_point = cp, .length = 4, .ok = true};
    }

    return {};
}

/// Renders a float or double through cc::to_chars and hands the text to `emit`.
/// The stack buffer covers the shortest form and every default precision; a precision large enough to outgrow it is
/// rare enough that one allocation beats sizing every frame for the worst case.
template <class T, class Emit>
void render_float(Emit&& emit, T v, cc::float_notation notation, int precision)
{
    auto const needed = cc::to_chars_size(notation, precision);
    if (needed <= cc::to_chars_float_max) [[likely]]
    {
        char buf[cc::to_chars_float_max];
        emit(cc::string_view(buf, cc::to_chars(buf, v, notation, precision)));
        return;
    }

    auto scratch = cc::vector<char>::create_uninitialized(needed);
    emit(cc::string_view(scratch.data(), cc::to_chars(scratch, v, notation, precision)));
}
} // namespace

// -------------------------------------------------------------------------------------------------
// the low-level emitters

void json::writer::impl_emit(cc::string_view text)
{
    if (_failed) // sticky: every write after the first failure is a no-op
        return;

    auto r = _out->write(cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size()));
    if (!r.has_value())
    {
        _failed = true;
        _error = cc::move(r).error();
    }
}

void json::writer::impl_misuse(char const* what)
{
    // Assert first: where assertions are on, a programming error should stop at the site with a stack behind it,
    // which is strictly more than finish() can tell anyone later.
    //
    // The assert is the contract as it stands today, and starting strict is deliberate: dropping it later leaves every
    // caller written against it working, while adding it back would kill programs that had come to rely on the error.
    CC_ASSERT(false, what);

    this->impl_fail(cc::format("json writer: {}", what));
}

void json::writer::impl_fail(cc::string message)
{
    if (_failed) // the first failure is the one worth reporting; the rest are its consequences
        return;

    _failed = true;
    _error = cc::any_error(cc::move(message));
}

void json::writer::impl_indent()
{
    if (_opts.indent <= 0 || _opts.newline_delimited || _compact_depth > 0)
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
    (void)this->impl_expect(_stack.empty(), "a root value is only written outside every scope");
    (void)this->impl_expect(!_finished, "the writer is already finished");
    (void)this->impl_expect(_root_count == 0 || _opts.newline_delimited,
                            "a JSON document holds one root value; set write_options::newline_delimited for several");

    if (_root_count > 0)
        this->impl_emit("\n");
    ++_root_count;
}

bool json::writer::impl_begin_member(cc::string_view key, i32 depth)
{
    if (!this->impl_expect(!_stack.empty(), "wrote a keyed value while no scope is open"))
        return false;
    if (!this->impl_expect(i32(_stack.size()) == depth, "wrote through a scope whose child is still open"))
        return false;
    if (!this->impl_expect(_stack.back().is_object, "wrote a key into an array scope"))
        return false;

    auto& lvl = _stack.back();
    if (lvl.has_any)
        this->impl_emit(",");
    lvl.has_any = true;

    this->impl_indent();
    this->impl_string(key, _opts.escape_non_ascii);
    this->impl_emit(_opts.indent > 0 && !_opts.newline_delimited && _compact_depth == 0 ? ": " : ":");
    return true;
}

bool json::writer::impl_begin_element(i32 depth)
{
    if (!this->impl_expect(!_stack.empty(), "wrote an array element while no scope is open"))
        return false;
    if (!this->impl_expect(i32(_stack.size()) == depth, "wrote through a scope whose child is still open"))
        return false;
    if (!this->impl_expect(!_stack.back().is_object, "wrote an unkeyed value into an object scope"))
        return false;

    auto& lvl = _stack.back();
    if (lvl.has_any)
        this->impl_emit(",");
    lvl.has_any = true;

    this->impl_indent();
    return true;
}

bool json::writer::impl_begin_value()
{
    if (_stack.empty()) // a bare scalar, or an object / array, as the whole document
    {
        this->impl_begin_root();
        return true;
    }

    return this->impl_begin_element(this->impl_depth());
}

void json::writer::impl_end(bool is_object)
{
    if (!this->impl_expect(!_stack.empty(), "closed a JSON scope while none is open"))
        return;
    if (!this->impl_expect(_stack.back().is_object == is_object,
                           is_object ? "end_object() closed an array scope" : "end_array() closed an object scope"))
        return;

    this->impl_close(this->impl_depth());
}

void json::writer::impl_open(bool is_object, layout l)
{
    this->impl_emit(is_object ? "{" : "[");
    _stack.push_back({.is_object = is_object, .has_any = false});

    if (l == layout::compact && _compact_depth == 0) // an inner compact scope is already covered by the outer one
        _compact_depth = i32(_stack.size());
}

void json::writer::impl_close(i32 depth)
{
    if (i32(_stack.size()) < depth) // already closed by finish()
        return;
    if (!this->impl_expect(i32(_stack.size()) == depth, "closed a JSON scope that still has an open child"))
        return;

    auto const lvl = _stack.back();
    auto const ends_compact = _compact_depth == depth;
    _stack.pop_back();
    if (lvl.has_any)
        this->impl_indent(); // the closing bracket lines up with what opened it, one level out
    this->impl_emit(lvl.is_object ? "}" : "]");

    if (ends_compact) // ...after the bracket, so the compact scope's own closing stays on the line
        _compact_depth = 0;
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
    auto const magnitude = v < 0 ? u64(-(v + 1)) + 1 : u64(v); // negation without overflowing at i64's minimum
    char buf[cc::to_chars_int_max];
    this->impl_integer(cc::string_view(buf, cc::to_chars(buf, v)), magnitude);
}

void json::writer::impl_u64(u64 v)
{
    char buf[cc::to_chars_int_max];
    this->impl_integer(cc::string_view(buf, cc::to_chars(buf, v)), v);
}

/// Emits already-rendered digits, applying the large-integer policy to them.
/// One compare against a constant on the common path, which is nothing next to the to_chars that produced `digits`.
void json::writer::impl_integer(cc::string_view digits, u64 magnitude)
{
    if (magnitude <= k_exact_integer_max) [[likely]]
    {
        this->impl_emit(digits);
        return;
    }

    ++_report.large_integers;

    switch (_opts.large_integers)
    {
    case large_integer_policy::number:
        this->impl_emit(digits);
        return;
    case large_integer_policy::string:
        this->impl_emit("\""); // the digits need no escaping, so the quotes are the whole of it
        this->impl_emit(digits);
        this->impl_emit("\"");
        return;
    case large_integer_policy::error:
        this->impl_fail(cc::format("json writer: {} is past 2^53 and a JSON number cannot carry it exactly", digits));
        return;
    }
}

void json::writer::impl_double(double v, cc::float_notation notation, int precision)
{
    if (!is_finite(v))
    {
        ++_report.non_finite;

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

    render_float([this](cc::string_view text) { this->impl_emit(text); }, v, notation, precision);
}

void json::writer::impl_float(float v, cc::float_notation notation, int precision)
{
    if (!is_finite(double(v)))
    {
        this->impl_double(double(v), notation, precision); // one policy, one place, counted there
        return;
    }

    render_float([this](cc::string_view text) { this->impl_emit(text); }, v, notation, precision);
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
                ++_report.undecodable_bytes;
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

        if (c == '<' && _opts.escape_html) // so an embedded "</script>" cannot end the tag it sits in
        {
            flush_run(i);
            emit_unicode_escape(c);
            ++i;
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
// the imperative layer
//
// A misused call still opens or still closes: the error is already recorded, so everything after it is a no-op, and
// the structure the caller believes it is in stays the structure the writer is in.

void json::writer::begin_object(cc::string_view key, layout l)
{
    (void)this->impl_begin_member(key, this->impl_depth());
    this->impl_open(true, l);
}

void json::writer::begin_array(cc::string_view key, layout l)
{
    (void)this->impl_begin_member(key, this->impl_depth());
    this->impl_open(false, l);
}

void json::writer::begin_object(layout l)
{
    (void)this->impl_begin_value();
    this->impl_open(true, l);
}

void json::writer::begin_array(layout l)
{
    (void)this->impl_begin_value();
    this->impl_open(false, l);
}

void json::writer::end_object()
{
    this->impl_end(true);
}

void json::writer::end_array()
{
    this->impl_end(false);
}

void json::writer::write(cc::string_view key, double value, cc::float_notation notation, int precision)
{
    if (this->impl_begin_member(key, this->impl_depth()))
        this->impl_double(value, notation, precision);
}

void json::writer::write(cc::string_view key, float value, cc::float_notation notation, int precision)
{
    if (this->impl_begin_member(key, this->impl_depth()))
        this->impl_float(value, notation, precision);
}

void json::writer::write_ascii(cc::string_view key, cc::string_view value)
{
    if (this->impl_begin_member(key, this->impl_depth()))
        this->impl_string(value, true);
}

void json::writer::write_raw(cc::string_view key, cc::string_view fragment)
{
    if (this->impl_begin_member(key, this->impl_depth()))
        this->impl_raw(fragment);
}

void json::writer::write(double value, cc::float_notation notation, int precision)
{
    if (this->impl_begin_value())
        this->impl_double(value, notation, precision);
}

void json::writer::write(float value, cc::float_notation notation, int precision)
{
    if (this->impl_begin_value())
        this->impl_float(value, notation, precision);
}

void json::writer::write_ascii(cc::string_view value)
{
    if (this->impl_begin_value())
        this->impl_string(value, true);
}

void json::writer::write_raw(cc::string_view fragment)
{
    if (this->impl_begin_value())
        this->impl_raw(fragment);
}

// -------------------------------------------------------------------------------------------------
// the RAII layer, and finishing

json::object_writer json::writer::object(layout l)
{
    this->impl_begin_root();
    this->impl_open(true, l);
    return object_writer(this, i32(_stack.size()));
}

json::array_writer json::writer::array(layout l)
{
    this->impl_begin_root();
    this->impl_open(false, l);
    return array_writer(this, i32(_stack.size()));
}

// A misused opener still opens: the error is already recorded, so everything inside is a no-op, and the handle the
// caller gets back behaves — closing in order, writing nothing — instead of being a null it would crash on.
// The returned depth is the stack's own, so the close still matches whatever the open actually did.

json::object_writer json::object_writer::write_object(cc::string_view key, layout l)
{
    (void)_w->impl_begin_member(key, _depth);
    _w->impl_open(true, l);
    return object_writer(_w, _w->impl_depth());
}

json::array_writer json::object_writer::write_array(cc::string_view key, layout l)
{
    (void)_w->impl_begin_member(key, _depth);
    _w->impl_open(false, l);
    return array_writer(_w, _w->impl_depth());
}

json::object_writer json::array_writer::write_object(layout l)
{
    (void)_w->impl_begin_element(_depth);
    _w->impl_open(true, l);
    return object_writer(_w, _w->impl_depth());
}

json::array_writer json::array_writer::write_array(layout l)
{
    (void)_w->impl_begin_element(_depth);
    _w->impl_open(false, l);
    return array_writer(_w, _w->impl_depth());
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
        // Close before reporting, not after: impl_fail turns every later emit into a no-op, so a document still open
        // here would lose the very brackets that keep it parseable.
        auto const was_open = !_stack.empty();
        while (!_stack.empty())
            this->impl_close(i32(_stack.size()));
        if (_opts.newline_delimited && _root_count > 0)
            this->impl_emit("\n"); // every record on its own line, the last one included

        // Not an assert, unlike the rest of the structural checks: an assert has to fire where the mistake is, and
        // this one is only visible somewhere else entirely — including on the destructor's path, where an early
        // return must not take the program down.
        if (was_open)
            this->impl_fail("json writer: finish() with a scope still open; the brackets were closed for you, but the "
                            "document stops short of what the caller meant to write");

        _finished = true;

        if (!_failed)
        {
            auto r = _out->flush();
            if (!r.has_value())
            {
                _failed = true;
                _error = cc::move(r).error();
            }
        }
    }

    if (!_failed)
        return cc::unit{};

    // The message MOVES OUT with the first report, so a repeat call still fails — just without the detail, rather
    // than silently claiming the write went fine.
    if (_error.is_empty())
        return cc::error("json writer: the write already failed; the first finish() carried the reason");
    return cc::error(cc::move(_error));
}

json::writer::~writer()
{
    if (_finished)
        return;

    // Best-effort, because an early return must not be punished for skipping finish().
    // But an error nobody asked for is still an error, and the log is where it goes rather than nowhere.
    auto const r = this->finish();
    if (!r.has_value())
        CC_LOG_ERROR("json writer destroyed without finish(); its error would otherwise be lost: {}",
                     r.error().to_string());
}

cc::result<cc::string> json::string_writer::finish()
{
    CC_RETURN_IF_ERROR(_writer.finish());
    return _buffer.take();
}
