#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/options.hh>
#include <nexus/args/value.hh>

// =========================================================================================================
// One declared argument, with its type erased.
//
// Stateless thunks are function pointers rather than cc::unique_function: nearly all of them are, and one
// indirect call beats an allocation and a virtual dispatch for something a parse touches per token.
// The unique_functions below are what make a binding move-only, which is also why nothing copies one.
//
// Everything that references a binding does so by INDEX into the builder's vector, never by pointer — the
// vector reallocates on the next declaration, and a stored pointer would dangle silently.
// =========================================================================================================

namespace nx::impl
{
enum class binding_kind
{
    option,     // named, e.g. --force
    positional, // taken by position
    rest,       // everything after a bare --
};

/// One spelling of one argument.
/// Whether it is short is stored rather than derived from length, because `--x` is a legal long name and
/// deriving it would make that spelling unrepresentable.
struct arg_name
{
    cc::string text; // without dashes
    bool is_short = false;
    bool hidden = false;

    [[nodiscard]] cc::string display() const { return (is_short ? cc::string("-") : cc::string("--")) + text; }
};

struct binding
{
    binding_kind kind = binding_kind::option;

    /// Every spelling, in declaration order.
    /// `canonical` is what diagnostics name: the first long spelling when there is one, since `--jobs`
    /// reads better in a sentence than `-j`.
    cc::vector<arg_name> names;
    cc::string canonical;

    // Documentation, copied rather than viewed so a caller may build any of it at runtime.
    cc::string metavar;
    cc::string desc;
    cc::string help;
    cc::string group;
    cc::string env;
    cc::string deprecated;
    cc::string type_name;

    /// The bound variable's value at declaration time, already formatted.
    /// Snapshotted eagerly because help may be asked for after a parse has overwritten the variable, and
    /// because a `required` argument's variable may never have held anything valid at all.
    cc::string default_text;
    bool has_default_text = false;

    bool takes_value = false;
    bool is_bool = false; // no value in the space form; a value only via --name=value
    bool negatable = false;
    bool counting = false; // each occurrence increments instead of assigning
    bool required = false;
    bool hidden = false;
    bool accumulates = false; // a vector target: every occurrence appends
    bool is_global = false;   // accepted at any subcommand depth, not only before the command name
    complete_hint complete = complete_hint::automatic;

    // Arity, for a variadic positional.
    // `max_count < 0` means unbounded.
    isize min_count = 0;
    isize max_count = -1;

    void* target = nullptr;
    bool (*parse_fn)(void* target, cc::string_view token, cc::string& error) = nullptr;
    void (*set_bool_fn)(void* target, bool value) = nullptr;
    void (*add_count_fn)(void* target, isize delta) = nullptr;
    void (*enumerate_values_fn)(cc::vector<cc::string>& out) = nullptr;

    /// The value rule, erased to "check what is behind `target`".
    /// Runs per occurrence, right after conversion, so a failure can quote the token that caused it.
    cc::string validator_description;
    cc::unique_function<bool(void const* target, cc::string& error)> validate;

    cc::unique_function<void()> action;
    cc::unique_function<void(cc::string_view)> value_action;
    cc::unique_function<void()> apply_make_default;

    // Parse state, reset at the start of every parse.
    isize occurrences = 0;
};

/// The bound variable's current value as text.
/// Only ever called for a cc::formattable T — a type without one simply shows no default in help rather
/// than failing to compile, which is what makes any type bindable.
template <class T>
cc::string format_default_of(T const& value)
{
    return cc::format("{}", value);
}

/// Fill in everything about `b` that depends on T: the parse thunk, the type name, the default snapshot.
/// `snapshot_default` is false for a required argument, whose variable may be uninitialized — reading it
/// to format a default nobody will print is the one way this could invoke undefined behaviour.
template <class T>
void bind_scalar(binding& b, T& target, bool snapshot_default)
{
    b.target = &target;
    b.type_name = cc::string(nx::custom::arg_value_trait<T>::type_name());

    b.parse_fn = [](void* t, cc::string_view token, cc::string& error) -> bool
    { return nx::custom::arg_value_trait<T>::parse(token, *static_cast<T*>(t), error); };

    if constexpr (std::same_as<T, bool>)
    {
        b.is_bool = true;
        b.set_bool_fn = [](void* t, bool value) { *static_cast<bool*>(t) = value; };
    }

    if constexpr (nx::enumerable_arg_value<T>)
        b.enumerate_values_fn = [](cc::vector<cc::string>& out) { nx::custom::arg_value_trait<T>::values(out); };

    if constexpr (cc::formattable<T>)
    {
        if (snapshot_default)
        {
            b.default_text = format_default_of(target);
            b.has_default_text = true;
        }
    }
}

/// A vector target: every occurrence appends rather than replacing.
/// No default is snapshotted — "the list starts with these two entries" reads as a lie in a help table.
template <class T>
void bind_vector(binding& b, cc::vector<T>& target)
{
    b.target = &target;
    b.accumulates = true;
    b.type_name = cc::string(nx::custom::arg_value_trait<T>::type_name());

    b.parse_fn = [](void* t, cc::string_view token, cc::string& error) -> bool
    {
        auto& out = *static_cast<cc::vector<T>*>(t);
        auto value = T{};
        if (!nx::custom::arg_value_trait<T>::parse(token, value, error))
            return false;

        out.push_back(cc::move(value));
        return true;
    };

    if constexpr (nx::enumerable_arg_value<T>)
        b.enumerate_values_fn = [](cc::vector<cc::string>& out) { nx::custom::arg_value_trait<T>::values(out); };
}

/// The T-independent half of arg_options.
/// Lets the declaration path stop being a template one call in, so builder.cc holds the logic and
/// builder.hh holds only the thin typed fronts.
struct common_options
{
    cc::string_view desc;
    cc::string_view help;
    cc::string_view metavar;
    cc::string_view group;
    cc::string_view env;
    cc::string_view default_text;
    cc::string_view deprecated;
    bool required = false;
    bool negatable = false;
    bool hidden = false;
    complete_hint complete = complete_hint::automatic;
    isize min_count = 0;
    isize max_count = -1;
};

template <class T>
[[nodiscard]] common_options to_common(arg_options<T> const& opts)
{
    return {
        .desc = opts.desc,
        .help = opts.help,
        .metavar = opts.metavar,
        .group = opts.group,
        .env = opts.env,
        .default_text = opts.default_text,
        .deprecated = opts.deprecated,
        .required = opts.required,
        .negatable = opts.negatable,
        .hidden = opts.hidden,
        .complete = opts.complete,
        .min_count = opts.min_count,
        .max_count = opts.max_count,
    };
}

/// Erase `make_default` down to "write the target", which is all the parse needs to know.
template <class T>
void bind_make_default(binding& b, T& target, arg_options<T>& opts)
{
    if (!opts.make_default.is_valid())
        return;

    b.apply_make_default = [fn = cc::move(opts.make_default), p = &target]() { *p = fn(); };
}

/// Erase a typed value rule down to "check what is behind the target pointer".
template <class T>
void bind_validator(binding& b, T& target, arg_options<T>& opts)
{
    if (!opts.validate.is_valid())
        return;

    b.validator_description = opts.validate.description;
    b.validate = [check = cc::move(opts.validate.check)](void const* t, cc::string& error)
    { return check(*static_cast<T const*>(t), error); };
}

/// The same for a vector target, where the rule is about each ELEMENT rather than about the list.
/// Checked against the element just appended, so the diagnostic names the token that failed.
template <class T>
void bind_element_validator(binding& b, arg_options<T>& opts)
{
    if (!opts.validate.is_valid())
        return;

    b.validator_description = opts.validate.description;
    b.validate = [check = cc::move(opts.validate.check)](void const* t, cc::string& error)
    {
        auto const& values = *static_cast<cc::vector<T> const*>(t);
        return values.empty() || check(values.back(), error);
    };
}

/// A counting flag: `-vvv` is three occurrences and the target ends up 3.
template <class T>
void bind_counter(binding& b, T& target)
{
    b.target = &target;
    b.counting = true;
    b.type_name = "COUNT";
    b.add_count_fn = [](void* t, isize delta) { *static_cast<T*>(t) = T(*static_cast<T*>(t) + delta); };

    if constexpr (cc::formattable<T>)
    {
        b.default_text = format_default_of(target);
        b.has_default_text = true;
    }
}

} // namespace nx::impl
