#pragma once

#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

#include <utility>

// cc::format for sequences, and join(): string/format.hh includes this at its end, so both arrive with cc::format.
//
//   cc::println("{}", v.values());               // [1, 2, 3]
//   cc::println("{:.2f}", ws.values());          // the spec applies to each element: [0.50, 1.25]
//   cc::println("{}", cc::zip(a, b));            // a zip's items print as tuples: [(1, 4), (2, 5)]
//
// Formatting a sequence consumes it, as every terminal operation does, so it must be an rvalue:
// cc::println("{}", s) on a named s does not compile, and cc::println("{}", cc::move(s)) spends it.

template <class S>
struct cc::impl::seq_format_support
{
    template <class X>
    static void format_element(cc::format_sink const& out, cc::string_view spec, X const& x)
    {
        if constexpr (seq_is_pack<X>)
        {
            out.put("(");
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
                ((Is == 0 ? void() : out.put(", "),
                  seq_format_support::format_element(out, spec, X::template item<Is>(x))),
                 ...);
            }(std::make_index_sequence<std::tuple_size_v<X>>{});
            out.put(")");
        }
        else
            cc::impl::format_one(out, spec, x);
    }

    // a named helper rather than a lambda: MSVC refuses a generic lambda called inside a consteval function
    template <class X, std::size_t... Is>
    static consteval void validate_pack(cc::string_view spec, std::index_sequence<Is...>)
    {
        (seq_format_support::validate_element<std::remove_cvref_t<decltype(X::template item<Is>(std::declval<X const&>()))>>(
             spec),
         ...);
    }

    template <class X>
    static consteval void validate_element(cc::string_view spec)
    {
        if constexpr (seq_is_pack<X>)
            seq_format_support::validate_pack<X>(spec, std::make_index_sequence<std::tuple_size_v<X>>{});
        else
            cc::impl::format_validate_spec_for<X>(spec);
    }

    // every element through the fold directly: a callback would spread a one-item pack, which then loses its parentheses
    template <class StepF>
    static void each_element(cc::sequence<S>&& s, StepF&& step)
    {
        static_assert(!cc::sequence<S>::is_infinite, "formatting an infinite sequence never ends: .take(n) first");
        auto source = seq_access::source(cc::move(s));
        (void)impl::seq_try_fold(source,
                                 [&](auto const& x)
                                 {
                                     step(x);
                                     return false;
                                 });
    }

    static void format(cc::format_sink const& out, cc::string_view spec, cc::sequence<S>&& s)
    {
        out.put("[");
        bool first = true;
        seq_format_support::each_element(cc::move(s),
                                         [&](auto const& x)
                                         {
                                             if (!first)
                                                 out.put(", ");
                                             first = false;
                                             seq_format_support::format_element(out, spec, x);
                                         });
        out.put("]");
    }

    template <class SeparatorT>
    static cc::string join(cc::sequence<S>&& s, SeparatorT const& separator)
    {
        auto result = cc::string();
        auto const sink = cc::format_sink{&result, [](void* ctx, char const* data, isize size)
                                          { static_cast<cc::string*>(ctx)->append(cc::string_view(data, size)); }};
        bool first = true;
        seq_format_support::each_element(cc::move(s),
                                         [&](auto const& x)
                                         {
                                             if (!first)
                                                 cc::impl::format_one(sink, "", separator);
                                             first = false;
                                             seq_format_support::format_element(sink, "", x);
                                         });
        return result;
    }
};

template <class S>
struct cc::custom::formatter<cc::sequence<S>>
{
    // format_validate_category refuses an lvalue or const argument, so the one format() receives is a non-const rvalue
    static constexpr bool consumes_argument = true;

    static consteval void validate(cc::string_view spec)
    {
        cc::impl::seq_format_support<S>::template validate_element<std::remove_cvref_t<typename cc::sequence<S>::item_t>>(
            spec);
    }

    static void format(cc::format_sink out, cc::string_view spec, cc::sequence<S> const& s)
    {
        cc::impl::seq_format_support<S>::format(out, spec, cc::move(const_cast<cc::sequence<S>&>(s)));
    }
};
