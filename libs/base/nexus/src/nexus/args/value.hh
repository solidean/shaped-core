#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/args/fwd.hh>

#include <concepts>
#include <type_traits>

// =========================================================================================================
// How one value type is parsed out of a command-line token, and how it is described in help.
//
// A type opts in through a `nx::custom::arg_value_trait<T>` specialization.
// The built-ins below cover bool, every integer type, float/double and the string types.
//
// A trait must provide:
//   static bool parse(cc::string_view token, T& out, cc::string& error);   // false plus a reason, never an assert
//   static cc::string_view type_name();                                    // the default metavar, e.g. "INT"
// and may provide:
//   static void values(cc::vector<cc::string>& out);                       // every accepted spelling
//
// `values` is what turns an enum into "one of: fast, slow" in help and into a completion candidate list,
// so a type with a small closed set should offer it.
//
// The error string is read by a user who mistyped, so it says what was expected rather than what failed:
// "expected an integer" beats naming the function that returned an error code.
// Leaving it empty is allowed, and a generic message is used instead.
//
// Formatting a DEFAULT value does not go through this trait — cc::format owns that, so a bindable type
// also needs a cc::custom::formatter for its default to appear in the help.
// =========================================================================================================

namespace nx::custom
{
/// The primary template is intentionally incomplete: specialize it for your type to opt in.
template <class T>
struct arg_value_trait;
} // namespace nx::custom

namespace nx
{
/// True when `T` can be bound to an argument, i.e. some arg_value_trait<T> defines the required members.
template <class T>
concept arg_value = requires(cc::string_view token, T& out, cc::string& error) {
    { nx::custom::arg_value_trait<T>::parse(token, out, error) } -> std::same_as<bool>;
    { nx::custom::arg_value_trait<T>::type_name() } -> std::convertible_to<cc::string_view>;
};

/// True when `T` publishes the closed set of spellings it accepts, which drives help and completion.
template <class T>
concept enumerable_arg_value
    = arg_value<T> && requires(cc::vector<cc::string>& out) { nx::custom::arg_value_trait<T>::values(out); };

} // namespace nx

namespace nx::impl
{
// The shared parsers behind the built-in traits, defined in value.cc so binding an int instantiates nothing.
bool parse_bool_value(cc::string_view token, bool& out, cc::string& error);
void bool_values(cc::vector<cc::string>& out);

bool parse_color_mode_value(cc::string_view token, cc::console::color_mode& out, cc::string& error);
void color_mode_values(cc::vector<cc::string>& out);

bool parse_signed_value(cc::string_view token, i64& out, cc::string& error, i64 min, i64 max);
bool parse_unsigned_value(cc::string_view token, u64& out, cc::string& error, u64 max);
bool parse_float_value(cc::string_view token, f32& out, cc::string& error);
bool parse_double_value(cc::string_view token, f64& out, cc::string& error);

/// The integer types nx::args binds numerically.
/// `char` is excluded on purpose: whether it is a number or a letter is exactly the ambiguity an argument
/// should not carry, so a caller who wants one takes a string and looks at its first byte.
template <class T>
concept bindable_integer
    = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, char8_t>
   && !std::same_as<T, char16_t> && !std::same_as<T, char32_t> && !std::same_as<T, wchar_t>;

// Computed rather than taken from <limits>, so the header stays free of it.
// The unsigned round-trip is what keeps this defined behaviour at every width, including i64 itself.
template <class T>
inline constexpr T integer_max = T(std::make_unsigned_t<T>(-1) >> (std::is_signed_v<T> ? 1 : 0));

template <class T>
inline constexpr T integer_min = std::is_signed_v<T> ? T(-integer_max<T> - 1) : T(0);

} // namespace nx::impl

/// bool accepts the CLI spellings, not only cc::to_string's, so `--color=yes` and `--color=1` both work.
/// Deliberately looser than cc::from_string<bool>, which is strictly to_string's inverse.
template <>
struct nx::custom::arg_value_trait<bool>
{
    static bool parse(cc::string_view token, bool& out, cc::string& error)
    {
        return nx::impl::parse_bool_value(token, out, error);
    }
    static cc::string_view type_name() { return "BOOL"; }
    static void values(cc::vector<cc::string>& out) { nx::impl::bool_values(out); }
};

/// Every signed integer type, each parsed against its own range so an overflow names the declared type.
template <class T>
    requires(nx::impl::bindable_integer<T> && std::is_signed_v<T>)
struct nx::custom::arg_value_trait<T>
{
    static bool parse(cc::string_view token, T& out, cc::string& error)
    {
        auto value = nx::i64(0);
        if (!nx::impl::parse_signed_value(token, value, error, nx::i64(nx::impl::integer_min<T>),
                                          nx::i64(nx::impl::integer_max<T>)))
            return false;

        out = T(value);
        return true;
    }
    static cc::string_view type_name() { return "INT"; }
};

/// Every unsigned integer type.
/// A leading '-' is rejected rather than wrapping to a huge positive number.
template <class T>
    requires(nx::impl::bindable_integer<T> && !std::is_signed_v<T>)
struct nx::custom::arg_value_trait<T>
{
    static bool parse(cc::string_view token, T& out, cc::string& error)
    {
        auto value = nx::u64(0);
        if (!nx::impl::parse_unsigned_value(token, value, error, nx::u64(nx::impl::integer_max<T>)))
            return false;

        out = T(value);
        return true;
    }
    static cc::string_view type_name() { return "UINT"; }
};

template <>
struct nx::custom::arg_value_trait<float>
{
    static bool parse(cc::string_view token, float& out, cc::string& error)
    {
        return nx::impl::parse_float_value(token, out, error);
    }
    static cc::string_view type_name() { return "FLOAT"; }
};

template <>
struct nx::custom::arg_value_trait<double>
{
    static bool parse(cc::string_view token, double& out, cc::string& error)
    {
        return nx::impl::parse_double_value(token, out, error);
    }
    static cc::string_view type_name() { return "FLOAT"; }
};

/// Any token is a valid string, an empty one included, so this never fails.
template <>
struct nx::custom::arg_value_trait<cc::string>
{
    static bool parse(cc::string_view token, cc::string& out, cc::string&)
    {
        out = cc::string(token);
        return true;
    }
    static cc::string_view type_name() { return "STRING"; }
};

/// Binding a string_view is safe: the parser copies every token into an arena the builder owns.
template <>
struct nx::custom::arg_value_trait<cc::string_view>
{
    static bool parse(cc::string_view token, cc::string_view& out, cc::string&)
    {
        out = token;
        return true;
    }
    static cc::string_view type_name() { return "STRING"; }
};

/// `--color auto|always|never`, so the mode is a value type rather than three hand-compared strings.
///
/// Here rather than in each tool that wants it: a specialization for a clean-core type, written privately in
/// two different .cc files, is an ODR violation waiting for the two to disagree.
/// nexus is the lowest library that knows both `cc::console::color_mode` and this trait, so it is the one
/// place the specialization can live once.
template <>
struct nx::custom::arg_value_trait<cc::console::color_mode>
{
    static bool parse(cc::string_view token, cc::console::color_mode& out, cc::string& error)
    {
        return nx::impl::parse_color_mode_value(token, out, error);
    }
    static cc::string_view type_name() { return "MODE"; }
    static void values(cc::vector<cc::string>& out) { nx::impl::color_mode_values(out); }
};
