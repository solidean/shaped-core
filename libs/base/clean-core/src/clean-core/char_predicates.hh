#pragma once

// =========================================================================================================
// Locale independent character predicates on 'char's
// =========================================================================================================
//
// see https://en.cppreference.com/w/cpp/string/byte
//
// Character classification:
//   is_space(c)         - whitespace character (space, \f, \t, \n, \r, \v)
//   is_blank(c)         - blank character (space, \t)
//   is_digit(c)         - decimal digit ('0' to '9')
//   is_hex_digit(c)     - hexadecimal digit ('0'-'9', 'a'-'f', 'A'-'F')
//   is_alphanumeric(c)  - letter or digit ('0'-'9', 'a'-'z', 'A'-'Z')
//   is_lower(c)         - lowercase letter ('a' to 'z')
//   is_upper(c)         - uppercase letter ('A' to 'Z')
//   is_punctuation(c)   - punctuation character
//   is_graphical(c)     - alphanumeric or punctuation character
//   is_printable(c)     - printable character (space, alphanumeric, or punctuation)
//   is_control(c)       - control character
//
// Character conversion:
//   to_lower(c)         - convert uppercase to lowercase (identity if not uppercase)
//   to_upper(c)         - convert lowercase to uppercase (identity if not lowercase)
//
// Predicate factories:
//   is_equal_fun(c)     - returns lambda that checks equality with c
//
// Predicate functors:
//   equal_case_sensitive           - functor for case-sensitive character equality
//   equal_case_insensitive         - functor for case-insensitive character equality
//   compare_ascii_case_sensitive   - functor for case-sensitive three-way comparison
//   compare_ascii_case_insensitive - functor for case-insensitive three-way comparison
//

namespace cc
{
// =========================================================================================================
// Character classification
// =========================================================================================================

/// Check if a character is whitespace
/// Matches: space, form feed, tab, newline, carriage return, vertical tab
/// Usage:
///   if (cc::is_space(' '))  // true
///   if (cc::is_space('a'))  // false
[[nodiscard]] constexpr bool is_space(char c)
{
    return c == ' ' || c == '\f' || c == '\t' || c == '\n' || c == '\r' || c == '\v';
}

/// Check if a character is blank
/// Matches: space, tab
/// Usage:
///   if (cc::is_blank(' '))  // true
///   if (cc::is_blank('\n')) // false
[[nodiscard]] constexpr bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

/// Check if a character is a decimal digit
/// Matches: '0' through '9'
/// Usage:
///   if (cc::is_digit('5'))  // true
///   if (cc::is_digit('a'))  // false
[[nodiscard]] constexpr bool is_digit(char c)
{
    return '0' <= c && c <= '9';
}

/// Check if a character is a hexadecimal digit
/// Matches: '0'-'9', 'a'-'f', 'A'-'F'
/// Usage:
///   if (cc::is_hex_digit('F'))  // true
///   if (cc::is_hex_digit('g'))  // false
[[nodiscard]] constexpr bool is_hex_digit(char c)
{
    return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F');
}

/// Check if a character is alphanumeric
/// Matches: '0'-'9', 'a'-'z', 'A'-'Z'
/// Usage:
///   if (cc::is_alphanumeric('a'))  // true
///   if (cc::is_alphanumeric('!'))  // false
[[nodiscard]] constexpr bool is_alphanumeric(char c)
{
    return ('0' <= c && c <= '9') || ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
}

/// Check if a character is lowercase
/// Matches: 'a' through 'z'
/// Usage:
///   if (cc::is_lower('a'))  // true
///   if (cc::is_lower('A'))  // false
[[nodiscard]] constexpr bool is_lower(char c)
{
    return 'a' <= c && c <= 'z';
}

/// Check if a character is uppercase
/// Matches: 'A' through 'Z'
/// Usage:
///   if (cc::is_upper('A'))  // true
///   if (cc::is_upper('a'))  // false
[[nodiscard]] constexpr bool is_upper(char c)
{
    return 'A' <= c && c <= 'Z';
}

/// Check if a character is punctuation
/// Matches ASCII punctuation characters
/// Ranges: 0x21-0x2F, 0x3A-0x40, 0x5B-0x60, 0x7B-0x7E
/// Usage:
///   if (cc::is_punctuation('!'))  // true
///   if (cc::is_punctuation('a'))  // false
[[nodiscard]] constexpr bool is_punctuation(char c)
{
    return ('\x21' <= c && c <= '\x2F') || ('\x3A' <= c && c <= '\x40') || ('\x5B' <= c && c <= '\x60')
        || ('\x7B' <= c && c <= '\x7E');
}

/// Check if a character is graphical
/// A graphical character is either alphanumeric or punctuation
/// Usage:
///   if (cc::is_graphical('a'))  // true
///   if (cc::is_graphical(' '))  // false
[[nodiscard]] constexpr bool is_graphical(char c)
{
    return is_alphanumeric(c) || is_punctuation(c);
}

/// Check if a character is printable
/// A printable character is either space, alphanumeric, or punctuation
/// Usage:
///   if (cc::is_printable(' '))  // true
///   if (cc::is_printable('\n')) // false
[[nodiscard]] constexpr bool is_printable(char c)
{
    return c == ' ' || is_alphanumeric(c) || is_punctuation(c);
}

/// Check if a character is a control character
/// Matches: 0x00-0x1F and 0x7F (DEL)
/// Usage:
///   if (cc::is_control('\n'))  // true
///   if (cc::is_control('a'))   // false
[[nodiscard]] constexpr bool is_control(char c)
{
    return ('\x00' <= c && c <= '\x1F') || c == '\x7F';
}

// =========================================================================================================
// Character conversion
// =========================================================================================================

/// Convert uppercase to lowercase
/// Returns the lowercase equivalent if c is uppercase, otherwise returns c unchanged
/// Usage:
///   char lower = cc::to_lower('A');  // 'a'
///   char same = cc::to_lower('5');   // '5'
[[nodiscard]] constexpr char to_lower(char c)
{
    return is_upper(c) ? char('a' + (c - 'A')) : c;
}

/// Convert lowercase to uppercase
/// Returns the uppercase equivalent if c is lowercase, otherwise returns c unchanged
/// Usage:
///   char upper = cc::to_upper('a');  // 'A'
///   char same = cc::to_upper('5');   // '5'
[[nodiscard]] constexpr char to_upper(char c)
{
    return is_lower(c) ? char('A' + (c - 'a')) : c;
}

// =========================================================================================================
// Predicate factories
// =========================================================================================================

/// Create a callable that checks equality with a specific character
/// Returns a lambda that captures c and compares it with the argument
/// Usage:
///   auto is_comma = cc::is_equal_fun(',');
///   if (is_comma(ch)) { ... }
[[nodiscard]] constexpr auto is_equal_fun(char c)
{
    return [c](char cc) { return c == cc; };
}

// =========================================================================================================
// Predicate functors
// =========================================================================================================

/// Functor for case-sensitive character equality comparison
/// Usage:
///   cc::equal_case_sensitive{}('a', 'a')  // true
///   cc::equal_case_sensitive{}('a', 'A')  // false
struct equal_case_sensitive
{
    [[nodiscard]] constexpr bool operator()(char a, char b) const { return a == b; }
};

/// Functor for case-insensitive character equality comparison
/// Converts both characters to lowercase before comparing
/// Only handles ASCII letters (a-z, A-Z)
/// Usage:
///   cc::equal_case_insensitive{}('a', 'A')  // true
///   cc::equal_case_insensitive{}('a', 'b')  // false
struct equal_case_insensitive
{
    [[nodiscard]] constexpr bool operator()(char a, char b) const { return to_lower(a) == to_lower(b); }
};

/// Functor for case-sensitive three-way character comparison
/// Returns:
///   < 0 if a < b
///   0 if a == b
///   > 0 if a > b
/// Usage:
///   cc::compare_ascii_case_sensitive{}('a', 'b')  // < 0
///   cc::compare_ascii_case_sensitive{}('b', 'a')  // > 0
///   cc::compare_ascii_case_sensitive{}('a', 'a')  // 0
struct compare_ascii_case_sensitive
{
    [[nodiscard]] constexpr int operator()(char a, char b) const { return int(a) - int(b); }
};

/// Functor for case-insensitive three-way character comparison
/// Converts both characters to lowercase before comparing
/// Only handles ASCII letters (a-z, A-Z)
/// Returns:
///   < 0 if to_lower(a) < to_lower(b)
///   0 if to_lower(a) == to_lower(b)
///   > 0 if to_lower(a) > to_lower(b)
/// Usage:
///   cc::compare_ascii_case_insensitive{}('A', 'a')  // 0
///   cc::compare_ascii_case_insensitive{}('a', 'B')  // < 0
///   cc::compare_ascii_case_insensitive{}('B', 'a')  // > 0
struct compare_ascii_case_insensitive
{
    [[nodiscard]] constexpr int operator()(char a, char b) const { return int(to_lower(a)) - int(to_lower(b)); }
};

} // namespace cc
