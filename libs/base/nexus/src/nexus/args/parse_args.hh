#pragma once

#include <clean-core/container/span.hh>
#include <nexus/args/builder.hh>

// =========================================================================================================
// The by-value path: an options STRUCT declares itself once, and one call fills it in.
//
// A thin wrapper over the by-reference builder rather than a second engine, so groups, validation,
// subcommands and everything else behave identically down both paths.
//
//     struct build_options
//     {
//         int jobs = 4;
//         bool verbose = false;
//
//         static void declare_args(nx::args_builder& args, build_options& self)
//         {
//             args.info({.name = "build", .description = "build the project"});
//             args.arg({"j", "jobs"}, self.jobs, "how many jobs to run at once");
//             args.arg({"v", "verbose"}, self.verbose, "print more");
//         }
//     };
//
//     auto const parsed = nx::parse_args<build_options>(argc, argv);
//     if (parsed.should_exit())
//         return parsed.exit_code();
//
//     run_build(parsed.value());
//
// `T` is default-constructed first, so the members' own initializers ARE the defaults — the same rule as
// the by-reference path, which is the main thing this protocol buys.
//
// Two tiers, mirroring cc::hash:
//   1. a `nx::custom::args_trait<T>` specialization — the override tier, checked first, for a type you do
//      not own or cannot change
//   2. a static member `T::declare_args(nx::args_builder&, T&)` — the tier for a type you do own
// =========================================================================================================

namespace nx::custom
{
/// The primary template is intentionally incomplete: specialize it to adapt a type you cannot change.
template <class T>
struct args_trait;
} // namespace nx::custom

namespace nx::impl
{
template <class>
inline constexpr bool args_dependent_false = false;

template <class T>
void declare_args_dispatch(args_builder& builder, T& value)
{
    if constexpr (requires { nx::custom::args_trait<T>::declare(builder, value); })
        nx::custom::args_trait<T>::declare(builder, value); // override tier
    else if constexpr (requires { T::declare_args(builder, value); })
        T::declare_args(builder, value); // the intrusive tier, for a type you own
    else
        static_assert(args_dependent_false<T>,
                      "nx::declare_args: T declares no arguments — add a 'static void declare_args("
                      "nx::args_builder&, T&)' or specialize nx::custom::args_trait<T>");
}

struct declare_args_fn
{
    template <class T>
    void operator()(args_builder& builder, T& value) const
    {
        declare_args_dispatch(builder, value);
    }
};
} // namespace nx::impl

namespace nx
{
/// Declare `value`'s arguments into `builder`, through whichever tier the type provides.
/// A niebloid, so it cannot be hijacked by an unrelated overload found through ADL.
inline constexpr impl::declare_args_fn declare_args = {};

/// True when `T` says how to declare itself, by either tier.
template <class T>
concept declares_args = requires(args_builder& b, T& v) { impl::declare_args_dispatch(b, v); };

} // namespace nx

/// What a by-value parse produced: the same three states as args_result, plus the filled-in struct.
///
/// `value()` is only meaningful when `ok()`. On any other outcome the struct holds whatever the parse got
/// to before it stopped, which is not something to act on.
template <class T>
class nx::parsed
{
public:
    parsed(args_result result, T value) : _result(cc::move(result)), _value(cc::move(value)) {}

    [[nodiscard]] bool ok() const { return _result.ok(); }
    [[nodiscard]] bool should_exit() const { return _result.should_exit(); }
    [[nodiscard]] int exit_code() const { return _result.exit_code(); }
    [[nodiscard]] args_result const& result() const { return _result; }

    [[nodiscard]] T const& value() const& { return _value; }
    [[nodiscard]] T&& value() && { return cc::move(_value); }

private:
    args_result _result;
    T _value;
};

namespace nx
{
/// Build a `T`, declare its arguments, parse `tokens` into it.
///
/// The builder lives only for the call, which is safe because every binding writes into the returned
/// struct and nothing keeps a view into the tokens past it — bind a cc::string rather than a
/// cc::string_view in a struct that outlives its parse.
template <declares_args T>
[[nodiscard]] parsed<T> parse_args(cc::span<cc::string_view const> tokens)
{
    auto value = T();
    auto builder = args(app_info{});
    declare_args(builder, value);

    auto result = builder.parse(tokens);
    return parsed<T>(cc::move(result), cc::move(value));
}

/// argv[0] is the program path and is skipped, exactly as args_builder::parse does.
template <declares_args T>
[[nodiscard]] parsed<T> parse_args(int argc, char const* const* argv)
{
    auto tokens = cc::vector<cc::string_view>();
    for (auto i = 1; i < argc; ++i)
        tokens.push_back(cc::string_view(argv[i]));

    return parse_args<T>(tokens);
}

} // namespace nx
