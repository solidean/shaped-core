#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/string_view.hh>

#include <type_traits>

// =========================================================================================================
// Compile-time format-string grammar: spec struct, scanner, and parser.
//
// This header is allocation-free and constexpr throughout. It is shared by two callers:
//   - the consteval cc::format_string constructor, which validates the string at compile time, and
//   - the runtime render loop in format.cc, which re-uses the very same parser so the two never disagree.
//
// The placeholder syntax follows Python / std::format / fmtlib:
//   replacement_field ::= '{' [arg_index] [':' format_spec] '}'
//   format_spec       ::= [[fill] align] [sign] ['#'] ['0'] [width] [grouping] ['.' precision] [type]
//   align ::= '<' | '>' | '^'      sign ::= '+' | '-' | ' '
//   grouping ::= one digit-group separator (any non-alphanumeric char except '.'); decimal groups by 3,
//                binary/hex/octal by 4. Numbers only. e.g. {:'} -> 1'234'567, {:,} -> 1,234,567
//   type  ::= d|x|X|o|b|B|c (ints) | f|F|e|E|g|G (floats) | s (string/bool) | p (ptr)
//   escapes: '{{' and '}}' produce a single literal brace.
// =========================================================================================================

// cc::impl is shared across clean-core, so the symbols here carry a format_ prefix to avoid collisions with
// other components: their bare names (field, type_tag, align_t, parse_spec, ...) would otherwise be generic.
namespace cc::impl
{
/// Reports an invalid format string.
///
/// At compile time (inside the consteval cc::format_string ctor) the throw is not a constant expression,
/// so reaching it turns into a compile error that surfaces the message. At runtime this path is only
/// reachable defensively (the string was already validated), so it routes through the clean-core
/// assertion handler instead of throwing.
[[noreturn]] constexpr void format_error(char const* msg)
{
    if (std::is_constant_evaluated())
        throw "cc::format: invalid format string"; // compile error; the message is shown via the throw below too
    else
    {
        cc::impl::handle_assert_failure("cc::format", msg, cc::source_location::current());
        cc::impl::perform_abort();
    }
}

/// Alignment of a value within its field width.
enum class format_align_t : char
{
    none,
    left,   // '<'
    right,  // '>'
    center, // '^'
};

/// Sign rendering policy for numbers.
enum class format_sign_t : char
{
    minus, // '-' (default): only negative numbers get a sign
    plus,  // '+': always show a sign
    space, // ' ': leading space for non-negative
};

/// Classifies an argument type so the validator can check spec-vs-type compatibility at compile time.
enum class format_type_tag : char
{
    other_user,  // user type: only the empty '{}' spec is accepted (unless it has a cc::custom::formatter)
    sint,        // signed integer
    uint,        // unsigned integer
    floating,    // float / double
    character,   // char
    boolean,     // bool
    string_like, // anything convertible to cc::string_view (incl. char const*)
    pointer,     // T* / nullptr_t (excluding char pointers, which are string_like)
};

/// A fully parsed format specification (the part after ':').
struct format_spec
{
    char fill = ' ';
    format_align_t align = format_align_t::none;
    format_sign_t sign = format_sign_t::minus;
    bool alternate = false;   // '#'
    bool zero_pad = false;    // '0'
    isize width = -1;         // -1 = unset
    char group = '\0';        // digit-group separator; '\0' = no grouping (e.g. '\'' -> 1'234'567)
    isize precision = -1;     // -1 = unset
    char presentation = '\0'; // type char; '\0' = default
};

/// A parsed replacement field.
///
/// The spec is kept as the raw text between ':' and '}' (not parsed): built-in types parse it with the
/// standard grammar (format_parse_spec), while a cc::custom::formatter<T> may interpret it however it likes.
struct format_field
{
    isize arg_index = -1;  // resolved (auto or explicit) argument index
    string_view spec_text; // raw spec text between ':' and '}' (empty if there was no ':')
    isize next_pos = 0;    // position right after the closing '}'
};

/// Tracks automatic-vs-explicit indexing across a single format string.
struct format_index_state
{
    isize next_auto = 0;
    bool saw_auto = false;
    bool saw_explicit = false;
};

/// Maps a (decayed) argument type to its format_type_tag. Used by the validator only.
template <class T>
consteval format_type_tag format_type_tag_of()
{
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>)
        return format_type_tag::boolean;
    else if constexpr (std::is_same_v<U, char>)
        return format_type_tag::character;
    else if constexpr (std::is_same_v<U, byte>)
        return format_type_tag::uint; // formatted as hex by format_value, but spec-compatible with unsigned
    else if constexpr (std::is_integral_v<U>)
        return std::is_signed_v<U> ? format_type_tag::sint : format_type_tag::uint;
    else if constexpr (std::is_floating_point_v<U>)
        return format_type_tag::floating;
    else if constexpr (std::is_convertible_v<U const&, cc::string_view>)
        return format_type_tag::string_like;
    else if constexpr (std::is_pointer_v<U> || std::is_same_v<U, nullptr_t>)
        return format_type_tag::pointer;
    else
        return format_type_tag::other_user; // e.g. a member to_string(); the empty '{}' spec only
}

/// Parses a format spec (the text between ':' and '}') into a format_spec.
/// Reports syntax errors via format_error. Does not validate against the argument type
/// (that is format_spec_error_for_type's job).
constexpr format_spec format_parse_spec(string_view body)
{
    format_spec spec;
    isize const n = body.size();
    isize p = 0;

    // [[fill] align] — a fill char is only recognized when followed by an alignment char
    auto align_of = [](char c) -> format_align_t
    {
        switch (c)
        {
        case '<':
            return format_align_t::left;
        case '>':
            return format_align_t::right;
        case '^':
            return format_align_t::center;
        default:
            return format_align_t::none;
        }
    };
    if (n - p >= 2 && align_of(body[p + 1]) != format_align_t::none)
    {
        spec.fill = body[p];
        spec.align = align_of(body[p + 1]);
        p += 2;
    }
    else if (p < n && align_of(body[p]) != format_align_t::none)
    {
        spec.align = align_of(body[p]);
        p += 1;
    }

    // [sign]
    if (p < n && (body[p] == '+' || body[p] == '-' || body[p] == ' '))
    {
        spec.sign = body[p] == '+' ? format_sign_t::plus : body[p] == ' ' ? format_sign_t::space : format_sign_t::minus;
        p += 1;
    }

    // ['#']
    if (p < n && body[p] == '#')
    {
        spec.alternate = true;
        p += 1;
    }

    // ['0'] — zero-padding (distinct from a width that merely starts with 0)
    if (p < n && body[p] == '0')
    {
        spec.zero_pad = true;
        p += 1;
    }

    // [width]
    if (p < n && is_digit(body[p]))
    {
        isize w = 0;
        while (p < n && is_digit(body[p]))
        {
            w = w * 10 + (body[p] - '0');
            if (w > 1'000'000)
                format_error("format width too large");
            p += 1;
        }
        spec.width = w;
    }

    // [grouping] — one digit-group separator: any non-alphanumeric char except '.' (precision) and braces.
    // Decimal groups by 3, binary/hex/octal by 4 (see format.cc). Letters stay the type; '.' stays precision.
    if (p < n && !is_alphanumeric(body[p]) && body[p] != '.' && body[p] != '{' && body[p] != '}')
    {
        spec.group = body[p];
        p += 1;
    }

    // ['.' precision]
    if (p < n && body[p] == '.')
    {
        p += 1;
        if (p >= n || !is_digit(body[p]))
            format_error("expected digits after '.' in format spec");
        isize prec = 0;
        while (p < n && is_digit(body[p]))
        {
            prec = prec * 10 + (body[p] - '0');
            if (prec > 1'000'000)
                format_error("format precision too large");
            p += 1;
        }
        spec.precision = prec;
    }

    // [type]
    if (p < n)
    {
        spec.presentation = body[p];
        p += 1;
    }

    if (p != n)
        format_error("invalid format spec");

    return spec;
}

/// Parses a single replacement field beginning at fmt[open] == '{'.
/// Resolves the argument index (auto or explicit), updating ix, and parses an optional spec.
/// Reports syntax / indexing-mode errors via format_error.
constexpr format_field format_parse_field(string_view fmt, isize open, format_index_state& ix)
{
    isize const n = fmt.size();
    isize p = open + 1; // past '{'

    format_field f;

    // [arg_index]
    if (p < n && is_digit(fmt[p]))
    {
        isize idx = 0;
        while (p < n && is_digit(fmt[p]))
        {
            idx = idx * 10 + (fmt[p] - '0');
            if (idx > 1'000'000)
                format_error("argument index too large");
            p += 1;
        }
        f.arg_index = idx;
        ix.saw_explicit = true;
    }
    else
    {
        f.arg_index = ix.next_auto++;
        ix.saw_auto = true;
    }
    if (ix.saw_auto && ix.saw_explicit)
        format_error("cannot mix automatic and explicit argument indexing");

    // [':' spec] — captured as raw text; interpreted later (format_parse_spec for built-ins, or a custom formatter)
    if (p < n && fmt[p] == ':')
    {
        p += 1;
        isize const spec_start = p;
        while (p < n && fmt[p] != '}')
            p += 1;
        if (p >= n)
            format_error("unterminated replacement field (missing '}')");
        f.spec_text = fmt.subview({.offset = spec_start, .size = p - spec_start});
    }

    if (p >= n || fmt[p] != '}')
        format_error("expected '}' to close replacement field");
    p += 1; // consume '}'

    f.next_pos = p;
    return f;
}

/// Checks whether a spec is valid for a given argument type tag.
/// Returns nullptr if valid, otherwise a human-readable error message.
constexpr char const* format_spec_error_for_type(format_spec const& s, format_type_tag tag)
{
    // user types: only the fully-default spec is allowed (no spec text)
    if (tag == format_type_tag::other_user)
    {
        bool const is_default = s.fill == ' ' && s.align == format_align_t::none && s.sign == format_sign_t::minus
                             && !s.alternate && !s.zero_pad && s.width == -1 && s.group == '\0' && s.precision == -1
                             && s.presentation == '\0';
        if (!is_default)
            return "user-defined types accept only '{}' (specialize cc::formatter<T> for custom specs)";
        return nullptr;
    }

    char const p = s.presentation;
    switch (tag)
    {
    case format_type_tag::sint:
    case format_type_tag::uint:
        if (!(p == '\0' || p == 'd' || p == 'b' || p == 'B' || p == 'o' || p == 'x' || p == 'X' || p == 'c'))
            return "invalid presentation type for integer (use d/b/B/o/x/X/c)";
        if (s.precision != -1)
            return "precision is not allowed for integers";
        break;
    case format_type_tag::character:
        if (!(p == '\0' || p == 'c' || p == 'd' || p == 'b' || p == 'B' || p == 'o' || p == 'x' || p == 'X'))
            return "invalid presentation type for char (use c/d/b/B/o/x/X)";
        if (s.precision != -1)
            return "precision is not allowed for char";
        if (s.group != '\0')
            return "digit grouping is not allowed for char";
        break;
    case format_type_tag::boolean:
        if (!(p == '\0' || p == 's' || p == 'd' || p == 'b' || p == 'B' || p == 'o' || p == 'x' || p == 'X'))
            return "invalid presentation type for bool (use s/d/b/B/o/x/X)";
        if (s.precision != -1)
            return "precision is not allowed for bool";
        if (s.group != '\0')
            return "digit grouping is not allowed for bool";
        break;
    case format_type_tag::floating:
        if (!(p == '\0' || p == 'f' || p == 'F' || p == 'e' || p == 'E' || p == 'g' || p == 'G'))
            return "invalid presentation type for floating point (use f/F/e/E/g/G)";
        break;
    case format_type_tag::string_like:
        if (!(p == '\0' || p == 's'))
            return "invalid presentation type for string (use s)";
        if (s.sign != format_sign_t::minus || s.alternate || s.zero_pad || s.group != '\0')
            return "sign / '#' / '0' / grouping are not allowed for strings";
        break;
    case format_type_tag::pointer:
        if (!(p == '\0' || p == 'p'))
            return "invalid presentation type for pointer (use p)";
        if (s.sign != format_sign_t::minus || s.alternate || s.zero_pad || s.precision != -1 || s.group != '\0')
            return "sign / '#' / '0' / precision / grouping are not allowed for pointers";
        break;
    default:
        break;
    }
    return nullptr;
}

/// Validates the structure of a format string at compile time, independent of argument types:
/// brace matching, '{{'/'}}' escapes, argument-index range, and no mixing of automatic/explicit indexing.
/// Per-type spec validation is done separately (see format.hh, which delegates to each argument's formatter).
constexpr void format_validate_structure(string_view fmt, isize arg_count)
{
    isize const n = fmt.size();
    isize pos = 0;
    format_index_state ix;

    while (pos < n)
    {
        char const c = fmt[pos];
        if (c == '{')
        {
            if (pos + 1 < n && fmt[pos + 1] == '{') // "{{" escape
            {
                pos += 2;
                continue;
            }
            format_field const f = format_parse_field(fmt, pos, ix);
            if (f.arg_index < 0 || f.arg_index >= arg_count)
                format_error("argument index out of range");
            pos = f.next_pos;
        }
        else if (c == '}')
        {
            if (pos + 1 < n && fmt[pos + 1] == '}') // "}}" escape
            {
                pos += 2;
                continue;
            }
            format_error("single '}' in format string (use '}}' for a literal brace)");
        }
        else
        {
            pos += 1;
        }
    }
}
} // namespace cc::impl
