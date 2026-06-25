#include "format.hh"

#include <charconv>

// This is the ONLY translation unit that includes <charconv>: it is the seam where value→text happens.
// A vendored number-formatting backend would replace just the chars_* definitions here.

namespace
{
// span-backed sink write: copy what fits, but always count the would-be length (snprintf semantics)
void span_sink_write(void* ctx, char const* data, cc::isize size)
{
    auto& st = *static_cast<cc::impl::span_sink_state*>(ctx);
    cc::isize const room = st.total < st.capacity ? st.capacity - st.total : 0;
    cc::isize const n = size < room ? size : room;
    for (cc::isize i = 0; i < n; ++i)
        st.data[st.total + i] = data[i];
    st.total += size;
}

void string_sink_write(void* ctx, char const* data, cc::isize size)
{
    auto& s = *static_cast<cc::string*>(ctx);
    if (size > 0)
        s.append(cc::string_view(data, size));
}

// shared float text production via std::to_chars; mode is 's' (shortest), 'f', 'e', or 'g'
cc::isize chars_from_float_impl(char* first, char* last, char mode, double v, float vf, bool is_double, cc::isize precision)
{
    std::to_chars_result r;
    if (mode == 's') // shortest round-trip
        r = is_double ? std::to_chars(first, last, v) : std::to_chars(first, last, vf);
    else
    {
        std::chars_format const fmt = mode == 'f' ? std::chars_format::fixed
                                    : mode == 'e' ? std::chars_format::scientific
                                                  : std::chars_format::general;
        r = is_double ? std::to_chars(first, last, v, fmt, int(precision))
                      : std::to_chars(first, last, vf, fmt, int(precision));
    }
    CC_ASSERT(r.ec == std::errc(), "cc::format: float buffer too small");
    return cc::isize(r.ptr - first);
}

char float_mode_of(char presentation)
{
    return presentation == '\0' ? 's' : char(cc::to_lower(presentation));
}

cc::isize float_precision_of(char mode, cc::isize precision)
{
    // f/e/g default to 6 digits of precision (matching std::format); shortest mode ignores precision
    if ((mode == 'f' || mode == 'e' || mode == 'g') && precision < 0)
        return 6;
    return precision;
}

// shared float rendering for both float and double; `n` is the seam-produced length in `buf`
void format_float_common(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, char* buf, cc::isize n)
{
    using namespace cc::impl;

    // uppercase variants: uppercase the seam output (affects e/E and inf/nan), then split sign
    if (spec.presentation == 'F' || spec.presentation == 'E' || spec.presentation == 'G')
        for (cc::isize i = 0; i < n; ++i)
            buf[i] = cc::to_upper(buf[i]);

    cc::string_view all(buf, n);
    bool neg = false;
    if (n > 0 && buf[0] == '-')
    {
        neg = true;
        all = all.subview(1);
    }
    write_decorated_number(sink, spec, neg, cc::string_view(), all);
}
} // namespace

namespace cc::impl
{
format_sink make_string_sink(cc::string& out)
{
    return format_sink{.ctx = static_cast<void*>(&out), .write = &string_sink_write};
}

format_sink make_span_sink(span_sink_state& state)
{
    return format_sink{.ctx = static_cast<void*>(&state), .write = &span_sink_write};
}

// -----------------------------------------------------------------------------------------------------
// value → raw text seam (std::to_chars)
// -----------------------------------------------------------------------------------------------------

isize chars_from_u64(span<char> buf, u64 v, int base, bool upper)
{
    char* const first = buf.data();
    char* const last = first + buf.size();
    auto const r = std::to_chars(first, last, v, base);
    CC_ASSERT(r.ec == std::errc(), "cc::format: integer buffer too small");
    isize const n = isize(r.ptr - first);
    if (upper)
        for (isize i = 0; i < n; ++i)
            first[i] = cc::to_upper(first[i]);
    return n;
}

isize chars_from_f32(span<char> buf, float v, char mode, isize precision)
{
    return chars_from_float_impl(buf.data(), buf.data() + buf.size(), mode, 0.0, v, /*is_double*/ false, precision);
}

isize chars_from_f64(span<char> buf, double v, char mode, isize precision)
{
    return chars_from_float_impl(buf.data(), buf.data() + buf.size(), mode, v, 0.0f, /*is_double*/ true, precision);
}

isize chars_from_ptr(span<char> buf, void const* p)
{
    CC_ASSERT(buf.size() >= 2, "cc::format: pointer buffer too small");
    buf[0] = '0';
    buf[1] = 'x';
    char* const mid = buf.data() + 2;
    char* const last = buf.data() + buf.size();
    auto const r = std::to_chars(mid, last, reinterpret_cast<uintptr_t>(p), 16);
    CC_ASSERT(r.ec == std::errc(), "cc::format: pointer buffer too small");
    return isize(r.ptr - buf.data());
}

// -----------------------------------------------------------------------------------------------------
// decoration
// -----------------------------------------------------------------------------------------------------

void write_decorated_text(format_sink const& sink, format_spec const& spec, string_view body, align_t default_align)
{
    if (spec.precision >= 0 && spec.precision < body.size())
        body = body.subview(0, spec.precision);

    align_t const a = spec.align == align_t::none ? default_align : spec.align;
    isize const pad = spec.width > body.size() ? spec.width - body.size() : 0;

    if (pad == 0)
    {
        sink.put(body);
        return;
    }

    switch (a)
    {
    case align_t::right:
        sink.put_repeat(spec.fill, pad);
        sink.put(body);
        break;
    case align_t::center:
    {
        isize const left = pad / 2;
        sink.put_repeat(spec.fill, left);
        sink.put(body);
        sink.put_repeat(spec.fill, pad - left);
        break;
    }
    case align_t::left:
    case align_t::none:
    default:
        sink.put(body);
        sink.put_repeat(spec.fill, pad);
        break;
    }
}

void write_decorated_number(format_sink const& sink,
                            format_spec const& spec,
                            bool negative,
                            string_view prefix,
                            string_view digits)
{
    string_view sign_sv;
    if (negative)
        sign_sv = cc::string_view("-");
    else if (spec.sign == sign_t::plus)
        sign_sv = cc::string_view("+");
    else if (spec.sign == sign_t::space)
        sign_sv = cc::string_view(" ");

    isize const core = sign_sv.size() + prefix.size() + digits.size();

    // zero-padding only applies when no explicit alignment was requested; the zeros go between the
    // prefix and the digits (e.g. "0x00ff", "-0003.14")
    if (spec.zero_pad && spec.align == align_t::none)
    {
        isize const zeros = spec.width > core ? spec.width - core : 0;
        sink.put(sign_sv);
        sink.put(prefix);
        sink.put_repeat('0', zeros);
        sink.put(digits);
        return;
    }

    align_t const a = spec.align == align_t::none ? align_t::right : spec.align;
    isize const pad = spec.width > core ? spec.width - core : 0;

    auto put_core = [&]
    {
        sink.put(sign_sv);
        sink.put(prefix);
        sink.put(digits);
    };

    if (pad == 0)
    {
        put_core();
        return;
    }

    switch (a)
    {
    case align_t::left:
        put_core();
        sink.put_repeat(spec.fill, pad);
        break;
    case align_t::center:
    {
        isize const left = pad / 2;
        sink.put_repeat(spec.fill, left);
        put_core();
        sink.put_repeat(spec.fill, pad - left);
        break;
    }
    case align_t::right:
    case align_t::none:
    default:
        sink.put_repeat(spec.fill, pad);
        put_core();
        break;
    }
}

void format_integer(format_sink const& sink, format_spec const& spec, bool negative, u64 magnitude)
{
    // 'c': emit the value as a single character
    if (spec.presentation == 'c')
    {
        char const c = char(magnitude);
        write_decorated_text(sink, spec, cc::string_view(&c, 1), align_t::left);
        return;
    }

    int base = 10;
    bool upper = false;
    string_view prefix;
    switch (spec.presentation)
    {
    case 'x':
        base = 16;
        prefix = spec.alternate ? cc::string_view("0x") : cc::string_view();
        break;
    case 'X':
        base = 16;
        upper = true;
        prefix = spec.alternate ? cc::string_view("0X") : cc::string_view();
        break;
    case 'o':
        base = 8;
        prefix = spec.alternate ? cc::string_view("0o") : cc::string_view();
        break;
    case 'b':
        base = 2;
        prefix = spec.alternate ? cc::string_view("0b") : cc::string_view();
        break;
    case 'B':
        base = 2;
        prefix = spec.alternate ? cc::string_view("0B") : cc::string_view();
        break;
    default: // 'd' or '\0'
        base = 10;
        break;
    }

    char buf[chars_int_max];
    isize const n = chars_from_u64(cc::span<char>(buf, chars_int_max), magnitude, base, upper);
    write_decorated_number(sink, spec, negative, prefix, cc::string_view(buf, n));
}

// -----------------------------------------------------------------------------------------------------
// render loop (shares the grammar parser in format_spec.hh with the compile-time validator)
// -----------------------------------------------------------------------------------------------------

void render(format_sink const& sink, string_view fmt, span<format_arg_entry const> entries)
{
    isize const n = fmt.size();
    isize pos = 0;
    isize lit_start = 0;
    index_state ix;

    while (pos < n)
    {
        char const c = fmt[pos];
        if (c == '{')
        {
            if (pos + 1 < n && fmt[pos + 1] == '{') // "{{" -> emit literal up to and including one '{'
            {
                sink.put(fmt.subview(lit_start, pos + 1 - lit_start));
                pos += 2;
                lit_start = pos;
                continue;
            }

            if (pos > lit_start)
                sink.put(fmt.subview(lit_start, pos - lit_start));

            field const f = parse_field(fmt, pos, ix);
            isize const idx = f.arg_index;
            if (idx >= 0 && idx < entries.size() && entries[idx].fn != nullptr)
                entries[idx].fn(sink, f.spec, entries[idx].ptr);
            else
                format_error("argument index out of range");

            pos = f.next_pos;
            lit_start = pos;
        }
        else if (c == '}')
        {
            if (pos + 1 < n && fmt[pos + 1] == '}') // "}}" -> emit literal up to and including one '}'
            {
                sink.put(fmt.subview(lit_start, pos + 1 - lit_start));
                pos += 2;
                lit_start = pos;
                continue;
            }
            format_error("single '}' in format string");
        }
        else
        {
            pos += 1;
        }
    }

    if (pos > lit_start)
        sink.put(fmt.subview(lit_start, pos - lit_start));
}
} // namespace cc::impl

// -----------------------------------------------------------------------------------------------------
// float formatters (need the seam, so defined here rather than in the header)
// -----------------------------------------------------------------------------------------------------

namespace cc
{
void formatter<float>::format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, float v)
{
    char const mode = float_mode_of(spec.presentation);
    cc::isize const prec = float_precision_of(mode, spec.precision);
    char buf[cc::impl::chars_float_max];
    cc::isize const n = cc::impl::chars_from_f32(cc::span<char>(buf, cc::impl::chars_float_max), v, mode, prec);
    format_float_common(sink, spec, buf, n);
}

void formatter<double>::format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, double v)
{
    char const mode = float_mode_of(spec.presentation);
    cc::isize const prec = float_precision_of(mode, spec.precision);
    char buf[cc::impl::chars_float_max];
    cc::isize const n = cc::impl::chars_from_f64(cc::span<char>(buf, cc::impl::chars_float_max), v, mode, prec);
    format_float_common(sink, spec, buf, n);
}
} // namespace cc
