#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/fwd.hh>

// =========================================================================================================
// What a value has to satisfy beyond parsing, stated once and used twice.
//
// A validator carries its own description, so the object that rejects a bad value is the one that prints
// the rule in help.
// That is the whole reason these are objects rather than lambdas: a predicate can only say no, while a
// validator can also say what the rule was.
//
// Two kinds, attached in two places:
//
//   VALUE validators are typed and run immediately after conversion, so the complaint can name the argument
//   and quote the token.
//   They go on arg_options::validate.
//
//   DOCUMENT validators are cross-argument rules — "one of these is required", "these two conflict".
//   They run once, after all binding, and go on the builder, because they are about the command line rather
//   than about any one argument.
//
// A document validator is skipped entirely when anything earlier already failed: a rule evaluated over a
// half-bound command line produces nonsense, and nonsense stacked on a real error is worse than silence.
// =========================================================================================================

/// One rule over one value of type `T`.
///
/// Rarely named directly: a factory from `nx::arg` converts into it, deducing `T` from the bound variable.
/// Construct one by hand when a rule is genuinely specific to your program.
template <class T>
struct nx::arg_validator
{
    /// How the rule reads in help, e.g. "must be in [1, 256]".
    /// Empty means it is not worth printing.
    cc::string description;

    /// Whether `value` passes.
    /// `error` may be filled in with something more specific than the description.
    cc::unique_function<bool(T const& value, cc::string& error)> check;

    [[nodiscard]] bool is_valid() const { return check.is_valid(); }
};

/// One rule over the whole command line, evaluated after every argument has been bound.
///
/// The rule is HANDED the builder rather than capturing it: a lambda holding `this` would dangle the moment
/// the builder moved, and building a command line in a factory that returns one is the house pattern.
struct nx::document_validator
{
    cc::string description;
    cc::unique_function<bool(args_builder const&)> check;
};

namespace nx::arg::impl
{
/// A bound that the argument's own type cannot represent makes the rule silently unsatisfiable — an i8
/// argument with `in_range(1, 256)` would enforce `1 <= v <= 0` while help still advertises 256.
/// A contract violation, so it asserts: the declaration is the program's own text, not the user's.
template <class T, class Bound>
void check_bound_fits(Bound const& bound)
{
    if constexpr (requires { bool(T(bound) == bound); })
        CC_ASSERT(T(bound) == bound, "nx::arg: this bound cannot be represented in the argument's type");
}

/// The lazy half of the design: a factory returns a spec, and the spec becomes an `arg_validator<T>` only
/// once `T` is known — which is where the argument is declared, not where the factory was called.
/// Without it, `nx::arg::in_range(1, 256)` would have to guess whether it bounds an int or a float.
template <class Derived>
struct validator_spec
{
    template <class T>
    operator nx::arg_validator<T>() const
    {
        return static_cast<Derived const&>(*this).template make<T>();
    }

    /// `a && b` passes only when both do, and reads as one rule in help.
    template <class Other>
    [[nodiscard]] auto operator&&(Other other) const;
};

template <class A, class B>
struct both_spec : validator_spec<both_spec<A, B>>
{
    A a;
    B b;

    template <class T>
    [[nodiscard]] nx::arg_validator<T> make() const
    {
        auto first = a.template make<T>();
        auto second = b.template make<T>();

        return {
            .description = cc::format("{} and {}", first.description, second.description),
            .check = [f = cc::move(first.check), s = cc::move(second.check)](T const& v, cc::string& error)
            { return f(v, error) && s(v, error); },
        };
    }
};

template <class Derived>
template <class Other>
auto validator_spec<Derived>::operator&&(Other other) const
{
    return both_spec<Derived, Other>{{}, static_cast<Derived const&>(*this), cc::move(other)};
}

/// `lo <= value <= hi`, inclusive at both ends.
template <class Lo, class Hi>
struct in_range_spec : validator_spec<in_range_spec<Lo, Hi>>
{
    Lo lo;
    Hi hi;

    template <class T>
    [[nodiscard]] nx::arg_validator<T> make() const
    {
        check_bound_fits<T>(lo);
        check_bound_fits<T>(hi);

        return {
            .description = cc::format("must be in [{}, {}]", lo, hi),
            .check = [lo = lo, hi = hi](T const& v, cc::string&) { return !(v < T(lo)) && !(T(hi) < v); },
        };
    }
};

template <class Bound>
struct at_least_spec : validator_spec<at_least_spec<Bound>>
{
    Bound bound;

    template <class T>
    [[nodiscard]] nx::arg_validator<T> make() const
    {
        check_bound_fits<T>(bound);

        return {
            .description = cc::format("must be at least {}", bound),
            .check = [bound = bound](T const& v, cc::string&) { return !(v < T(bound)); },
        };
    }
};

template <class Bound>
struct at_most_spec : validator_spec<at_most_spec<Bound>>
{
    Bound bound;

    template <class T>
    [[nodiscard]] nx::arg_validator<T> make() const
    {
        check_bound_fits<T>(bound);

        return {
            .description = cc::format("must be at most {}", bound),
            .check = [bound = bound](T const& v, cc::string&) { return !(T(bound) < v); },
        };
    }
};

/// The value must be one of a fixed set, which is also what the help lists.
struct one_of_spec : validator_spec<one_of_spec>
{
    cc::vector<cc::string> allowed;

    template <class T>
    [[nodiscard]] nx::arg_validator<T> make() const
    {
        auto listed = cc::string();
        for (auto const& value : allowed)
        {
            if (!listed.empty())
                listed += ", ";

            listed += value;
        }

        return {
            .description = cc::format("must be one of: {}", listed),
            .check =
                [allowed = allowed](T const& v, cc::string&)
            {
                auto const text = cc::format("{}", v);
                for (auto const& candidate : allowed)
                    if (candidate == text)
                        return true;

                return false;
            },
        };
    }
};

/// Rejects an empty string, which is the usual way an unset shell variable slips through as a value.
struct non_empty_spec : validator_spec<non_empty_spec>
{
    template <class T>
    [[nodiscard]] nx::arg_validator<T> make() const
    {
        return {
            .description = "must not be empty",
            .check = [](T const& v, cc::string&) { return !cc::string_view(v).empty(); },
        };
    }
};

/// Any rule of your own, with the description you want it to print.
template <class Fn>
struct predicate_spec : validator_spec<predicate_spec<Fn>>
{
    cc::string description;
    Fn fn;

    template <class T>
    [[nodiscard]] nx::arg_validator<T> make() const
    {
        return {
            .description = description,
            .check = [fn = fn](T const& v, cc::string&) { return fn(v); },
        };
    }
};

} // namespace nx::arg::impl

namespace nx::arg
{
// =========================================================================================================
// The value rules.
//
// Each derives its own description, so `nx::arg::in_range(1, 256)` needs no prose from the caller and the
// help line cannot drift from what is enforced.
// Compose with `&&`.
// =========================================================================================================

template <class Lo, class Hi>
[[nodiscard]] impl::in_range_spec<Lo, Hi> in_range(Lo lo, Hi hi)
{
    return {{}, lo, hi};
}

template <class Bound>
[[nodiscard]] impl::at_least_spec<Bound> at_least(Bound bound)
{
    return {{}, bound};
}

template <class Bound>
[[nodiscard]] impl::at_most_spec<Bound> at_most(Bound bound)
{
    return {{}, bound};
}

[[nodiscard]] impl::one_of_spec one_of(cc::span<cc::string_view const> allowed);

[[nodiscard]] inline impl::non_empty_spec non_empty()
{
    return {};
}

template <class Fn>
[[nodiscard]] impl::predicate_spec<Fn> satisfies(cc::string_view description, Fn fn)
{
    return {{}, cc::string(description), cc::move(fn)};
}

} // namespace nx::arg
