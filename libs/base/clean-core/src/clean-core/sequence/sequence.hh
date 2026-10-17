#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>
#include <clean-core/sequence/impl/adaptors.hh>
#include <clean-core/sequence/impl/call.hh>
#include <clean-core/sequence/impl/combinators.hh>
#include <clean-core/sequence/impl/factories.hh>
#include <clean-core/sequence/impl/windows.hh>
#include <clean-core/sequence/source.hh>

// Every container reaches this header, so be careful what it depends on.

/// The smallest and the largest element, from one pass; structured bindings take it apart: auto [lo, hi] = ...
template <class T>
struct cc::min_and_max
{
    T min;
    T max;
};

/// The elements split by a predicate, each half in sequence order.
template <class T>
struct cc::partition_result
{
    cc::vector<T> passed;
    cc::vector<T> failed;
};

/// A lazy, single-pass, eval-at-most-once sequence over a source, with functional compositions on top.
/// Containers hand one out through values() and indices(); see libs/base/clean-core/docs/sequence.md.
///
///   auto const n = v.values().filter([](int x) { return x > 0; }).count();
///   auto const s = v.values().assert_non_empty().sum();
///
/// A sequence references the container it came from and must not outlive it; it is a temporary, like span.
/// Terminal operations are &&-qualified and consume it, so `cc::move(s).count()` is how a named one is spent.
/// Every callback, keys and predicates included, may take a leading `isize` index, its item's position in the sequence
/// it is applied to, and receives a zip's or a window's items spread, f(a, b), where it takes them that way.
/// Where both fit, spread wins: f(isize i, auto w) over a zip of ints is f(a, b).
template <class SourceT>
struct cc::sequence
{
    static_assert(std::is_object_v<SourceT> && !std::is_const_v<SourceT>, "a sequence holds its source by value");

    //
    // traits & typedefs
    //
public:
    using source_t = SourceT;

    // what one step yields: T&, T const& or a prvalue T
    using item_t = typename SourceT::item_t;

    // what an item materializes as: "int&" -> int, a zip's pack -> a cc::tuple
    using element_t = typename impl::seq_element_of<item_t>::type;

    // pointer to an item, constness preserved; only meaningful with stable elements
    using element_ptr_t = std::add_pointer_t<std::remove_reference_t<item_t>>;

    // at least one element, known at compile time; an infinite sequence never runs out, so it is non-empty too
    static constexpr bool is_non_empty = impl::seq_is_non_empty<SourceT> || impl::seq_is_infinite<SourceT>;

    // items are references to address-stable storage
    static constexpr bool has_stable_elements = std::is_reference_v<item_t>;

    // those references stay valid after this sequence dies, so a pointer may escape it
    static constexpr bool has_borrowed_elements = has_stable_elements && impl::seq_is_borrowed<SourceT>;

    // the element count is available without traversal
    static constexpr bool has_known_size = impl::seq_has_size<SourceT>;

    // never runs out: a reduction that traverses every element does not compile, so take(n) first
    static constexpr bool is_infinite = impl::seq_is_infinite<SourceT>;

    // can be walked from the back, so reversed() and the last_* forms are available
    static constexpr bool is_reversible = impl::seq_has_next_back<SourceT>;

    //
    // reductions
    // (structure-consuming, value-producing)
    //
public:
    /// The number of elements.
    /// With a known size it evaluates nothing, so a side-effecting map is skipped; consume() runs everything.
    [[nodiscard]] constexpr isize count() &&
    {
        static_assert(!is_infinite, "count() of an infinite sequence never ends: .take(n) first");
        if constexpr (has_known_size)
            return _source.size();
        else
        {
            isize n = 0;
            (void)impl::seq_try_fold(_source,
                                     [&](auto&&)
                                     {
                                         ++n;
                                         return false;
                                     });
            return n;
        }
    }

    [[nodiscard]] constexpr isize count_if(auto&& pred) &&
    {
        static_assert(!is_infinite, "count_if() of an infinite sequence never ends: .take(n) first");
        isize n = 0;
        auto call = impl::seq_indexed_call<std::remove_reference_t<decltype(pred)>>{pred};
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     n += bool(call(cc::forward<X>(x))) ? 1 : 0;
                                     return false;
                                 });
        return n;
    }

    [[nodiscard]] constexpr bool any(auto&& pred) &&
    {
        auto call = impl::seq_indexed_call<std::remove_reference_t<decltype(pred)>>{pred};
        return impl::seq_try_fold(_source, [&]<class X>(X&& x) { return bool(call(cc::forward<X>(x))); });
    }

    [[nodiscard]] constexpr bool all(auto&& pred) &&
    {
        auto call = impl::seq_indexed_call<std::remove_reference_t<decltype(pred)>>{pred};
        return !impl::seq_try_fold(_source, [&]<class X>(X&& x) { return !bool(call(cc::forward<X>(x))); });
    }

    [[nodiscard]] constexpr bool none(auto&& pred) && { return !cc::move(*this).any(pred); }

    /// Whether some element equals v; a source that knows membership answers without a scan.
    template <class V>
    [[nodiscard]] constexpr bool contains(V const& v) &&
    {
        if constexpr (impl::seq_has_contains<SourceT, V>)
            return _source.contains(v);
        else
            return impl::seq_try_fold(_source, [&](auto const& x) { return bool(x == v); });
    }

    /// The position of the first element satisfying pred.
    [[nodiscard]] constexpr cc::optional<typename impl::seq_dependent<SourceT, isize>::type> index_of(auto&& pred) &&
    {
        isize idx = 0;
        auto call = impl::seq_indexed_call<std::remove_reference_t<decltype(pred)>>{pred};
        if (impl::seq_try_fold(_source,
                               [&]<class X>(X&& x)
                               {
                                   if (bool(call(cc::forward<X>(x))))
                                       return true;
                                   ++idx;
                                   return false;
                               }))
            return idx;
        return {};
    }

    /// A pointer to the first element satisfying pred, or nullptr.
    [[nodiscard]] constexpr element_ptr_t find(auto&& pred) &&
    {
        static_assert(has_borrowed_elements,
                      "find() returns a pointer, so it needs references that outlive the sequence: "
                      "a by-value map has none, and a sequence owning its container takes them with it");

        element_ptr_t result = nullptr;
        auto call = impl::seq_indexed_call<std::remove_reference_t<decltype(pred)>>{pred};
        (void)impl::seq_try_fold(_source,
                                 [&](item_t x)
                                 {
                                     if (!bool(call(x)))
                                         return false;
                                     result = &x;
                                     return true;
                                 });
        return result;
    }

    /// Calls f on each element; a return value is ignored, so this never stops early.
    constexpr void each(auto&& f) &&
    {
        static_assert(!is_infinite, "each() over an infinite sequence never ends: .take(n) first, or try_fold to stop");
        auto call = impl::seq_indexed_call<std::remove_reference_t<decltype(f)>>{f};
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     (void)call(cc::forward<X>(x));
                                     return false;
                                 });
    }

    /// Evaluates every element and discards it, so every side effect along the chain happens.
    constexpr void consume() &&
    {
        static_assert(!is_infinite, "consume() of an infinite sequence never ends: .take(n) first");
        (void)impl::seq_try_fold(_source, [](auto&&) { return false; });
    }

    /// The general early-out fold every reduction above is shaped like: f(idx?, item) -> bool|void, true stops.
    /// Returns whether f stopped it.
    constexpr bool try_fold(auto&& f) &&
    {
        auto call = impl::seq_indexed_call<std::remove_reference_t<decltype(f)>>{f};
        return impl::seq_try_fold(_source, [&]<class X>(X&& x) { return impl::seq_stops(call, cc::forward<X>(x)); });
    }

    //
    // reductions over a first element
    // (x() needs a non-empty sequence; x_or, try_x and sum_from take any)
    // x(f) is map(f).x(): it reduces the projected values
    //
public:
    /// The sum of all elements, accumulated in decltype(x + x): u8 elements sum as int.
    [[nodiscard]] constexpr auto sum() &&
    {
        static_assert(is_non_empty, "sum() needs a non-empty sequence: spell .assert_non_empty().sum(), branch on "
                                    ".try_as_non_empty(), or start from a value with .sum_from(init)");
        using sum_t = std::remove_cvref_t<decltype(std::declval<element_t const&>() + std::declval<element_t const&>())>;
        return this->_fold_from_first([]<class X>(X&& x) { return sum_t(cc::forward<X>(x)); }, //
                                      [](sum_t& acc, auto const& x) { impl::seq_add_to(acc, x); });
    }
    template <class F>
    [[nodiscard]] constexpr auto sum(F&& f) &&
    {
        return cc::move(*this).map(cc::forward<F>(f)).sum();
    }

    /// init plus the sum of all elements; init's type is the accumulator, so it must hold acc + x without conversion.
    template <class InitT>
    [[nodiscard]] constexpr std::remove_cvref_t<InitT> sum_from(InitT&& init) &&
    {
        using acc_t = std::remove_cvref_t<InitT>;
        static_assert(
            std::is_same_v<std::remove_cvref_t<decltype(std::declval<acc_t const&>() + std::declval<element_t const&>())>, acc_t>,
            "sum_from(init): acc + element must have init's type, or every step converts it "
            "(sum_from(0) over floats truncates); start from a value of the type the sum should have");
        static_assert(!is_infinite, "sum_from() of an infinite sequence never ends: .take(n) first");
        acc_t acc = cc::forward<InitT>(init);
        (void)impl::seq_try_fold(_source,
                                 [&](auto const& x)
                                 {
                                     impl::seq_add_to(acc, x);
                                     return false;
                                 });
        return acc;
    }

    /// The mean, sum / count, summed in AccT.
    /// By default integers sum in 64 bits and divide as f64, floating point stays in its own type, and any other type
    /// sums in decltype(x + x) and divides by its scalar_t: a sequence of tg::vec3f averages to a vec3f.
    /// average<f64>() sums floats in double precision.
    template <class AccT = void>
    [[nodiscard]] constexpr auto average() &&
    {
        static_assert(is_non_empty, "average() needs a non-empty sequence: spell .assert_non_empty().average(), "
                                    "branch on .try_as_non_empty(), or use .average_or(fallback) / .try_average()");
        return this->template _average<AccT>([](auto r) { return r; },
                                             [] { return typename _average_t<AccT>::result_t(); });
    }

    template <class AccT = void, class U>
    [[nodiscard]] constexpr auto average_or(U&& fallback) &&
    {
        using result_t = typename _average_t<AccT>::result_t;
        return this->template _average<AccT>([](result_t r) { return r; },
                                             [&] { return result_t(cc::forward<U>(fallback)); });
    }

    template <class AccT = void>
    [[nodiscard]] constexpr auto try_average() &&
    {
        using result_t = typename _average_t<AccT>::result_t;
        return this->template _average<AccT>([](result_t r) { return cc::optional<result_t>(cc::move(r)); },
                                             [] { return cc::optional<result_t>(); });
    }

    template <class F>
    [[nodiscard]] constexpr auto average(F&& f) &&
    {
        return cc::move(*this).map(cc::forward<F>(f)).average();
    }

    /// The smallest element by operator<, the first of equal ones, by value.
    [[nodiscard]] constexpr element_t min() &&
    {
        static_assert(is_non_empty, "min() needs a non-empty sequence: spell .assert_non_empty().min(), branch on "
                                    ".try_as_non_empty(), or use .min_or(fallback) / .try_min()");
        return this->_fold_from_first(_init_element(), _min_step());
    }
    template <class F>
    [[nodiscard]] constexpr auto min(F&& f) &&
    {
        return cc::move(*this).map(cc::forward<F>(f)).min();
    }

    /// The largest element by operator<, the first of equal ones, by value.
    [[nodiscard]] constexpr element_t max() &&
    {
        static_assert(is_non_empty, "max() needs a non-empty sequence: spell .assert_non_empty().max(), branch on "
                                    ".try_as_non_empty(), or use .max_or(fallback) / .try_max()");
        return this->_fold_from_first(_init_element(), _max_step());
    }
    template <class F>
    [[nodiscard]] constexpr auto max(F&& f) &&
    {
        return cc::move(*this).map(cc::forward<F>(f)).max();
    }

    template <class U>
    [[nodiscard]] constexpr element_t min_or(U&& fallback) &&
    {
        return this->_fold_from_first_or(
            _init_element(), _min_step(), [](element_t m) { return m; },
            [&] { return element_t(cc::forward<U>(fallback)); });
    }

    template <class U>
    [[nodiscard]] constexpr element_t max_or(U&& fallback) &&
    {
        return this->_fold_from_first_or(
            _init_element(), _max_step(), [](element_t m) { return m; },
            [&] { return element_t(cc::forward<U>(fallback)); });
    }

    [[nodiscard]] constexpr cc::optional<element_t> try_min() &&
    {
        return this->_fold_from_first_or(
            _init_element(), _min_step(), [](element_t m) { return cc::optional<element_t>(cc::move(m)); },
            [] { return cc::optional<element_t>(); });
    }

    [[nodiscard]] constexpr cc::optional<element_t> try_max() &&
    {
        return this->_fold_from_first_or(
            _init_element(), _max_step(), [](element_t m) { return cc::optional<element_t>(cc::move(m)); },
            [] { return cc::optional<element_t>(); });
    }

    /// The element whose key(idx?, item) is smallest by operator<, the first of equal ones, by value.
    template <class KeyF>
    [[nodiscard]] constexpr element_t min_by(KeyF&& key) &&
    {
        static_assert(is_non_empty, "min_by() needs a non-empty sequence: spell .assert_non_empty().min_by(key), "
                                    "or branch on .try_as_non_empty()");
        return this->template _best_by<true>(key);
    }

    /// The element whose key(idx?, item) is largest by operator<, the first of equal ones, by value.
    template <class KeyF>
    [[nodiscard]] constexpr element_t max_by(KeyF&& key) &&
    {
        static_assert(is_non_empty, "max_by() needs a non-empty sequence: spell .assert_non_empty().max_by(key), "
                                    "or branch on .try_as_non_empty()");
        return this->template _best_by<false>(key);
    }

    /// The first element, by value.
    [[nodiscard]] constexpr element_t first() &&
    {
        static_assert(is_non_empty, "first() needs a non-empty sequence: spell .assert_non_empty().first(), "
                                    "or use .first_or(fallback) / .try_first()");
        auto r = _source.next();
        bool const has_first = bool(r);
        CC_ASSERT(has_first, "a sequence typed non-empty was empty");
        [[assume(has_first)]];
        return sequence::_take(impl::seq_get(r));
    }

    template <class U>
    [[nodiscard]] constexpr element_t first_or(U&& fallback) &&
    {
        auto r = _source.next();
        if (!r)
            return element_t(cc::forward<U>(fallback));
        return sequence::_take(impl::seq_get(r));
    }

    [[nodiscard]] constexpr cc::optional<element_t> try_first() &&
    {
        auto r = _source.next();
        if (!r)
            return {};
        return cc::optional<element_t>(sequence::_take(impl::seq_get(r)));
    }

    /// The last element, by value; a source that steps from the back answers without a traversal.
    [[nodiscard]] constexpr element_t last() &&
    {
        static_assert(is_non_empty, "last() needs a non-empty sequence: spell .assert_non_empty().last(), "
                                    "or use .try_last()");
        if constexpr (is_reversible)
        {
            auto r = _source.next_back();
            bool const has_last = bool(r);
            CC_ASSERT(has_last, "a sequence typed non-empty was empty");
            [[assume(has_last)]];
            return sequence::_take(impl::seq_get(r));
        }
        else
            return this->_fold_from_first(_init_element(), _last_step());
    }

    [[nodiscard]] constexpr cc::optional<element_t> try_last() &&
    {
        if constexpr (is_reversible)
        {
            auto r = _source.next_back();
            if (!r)
                return {};
            return cc::optional<element_t>(sequence::_take(impl::seq_get(r)));
        }
        else
            return this->_fold_from_first_or(
                _init_element(), _last_step(), [](element_t m) { return cc::optional<element_t>(cc::move(m)); },
                [] { return cc::optional<element_t>(); });
    }

    /// A pointer to the last element satisfying pred(idx?, item), or nullptr; idx is the element's position from the front.
    [[nodiscard]] constexpr element_ptr_t find_last(auto&& pred) &&
    {
        static_assert(has_borrowed_elements, "find_last() returns a pointer, so it needs references that outlive the "
                                             "sequence, as find() does");
        static_assert(is_reversible, "find_last() walks from the back, which this sequence cannot");
        element_ptr_t result = nullptr;
        isize idx = this->_back_index<decltype(pred)>();
        (void)impl::seq_try_rfold(_source,
                                  [&](item_t x)
                                  {
                                      isize i = idx--;
                                      if (!bool(impl::seq_call(pred, i, x)))
                                          return false;
                                      result = &x;
                                      return true;
                                  });
        return result;
    }

    /// The position of the last element satisfying pred(idx?, item), counted from the front.
    [[nodiscard]] constexpr cc::optional<typename impl::seq_dependent<SourceT, isize>::type> last_index_of(auto&& pred) &&
    {
        static_assert(is_reversible && has_known_size, "last_index_of() walks from the back and counts from the front, "
                                                       "so it needs both");
        isize idx = _source.size() - 1;
        if (impl::seq_try_rfold(_source,
                                [&]<class X>(X&& x)
                                {
                                    isize i = idx;
                                    if (bool(impl::seq_call(pred, i, cc::forward<X>(x))))
                                        return true;
                                    --idx;
                                    return false;
                                }))
            return idx;
        return {};
    }

    /// The smallest and the largest element, in one pass, by value.
    [[nodiscard]] constexpr cc::min_and_max<element_t> min_max() &&
    {
        static_assert(is_non_empty, "min_max() needs a non-empty sequence: spell .assert_non_empty().min_max(), or "
                                    "use .try_min_max()");
        return this->_fold_from_first(_init_min_max(), _min_max_step());
    }

    [[nodiscard]] constexpr cc::optional<cc::min_and_max<element_t>> try_min_max() &&
    {
        using result_t = cc::min_and_max<element_t>;
        return this->_fold_from_first_or(
            _init_min_max(), _min_max_step(), [](result_t r) { return cc::optional<result_t>(cc::move(r)); },
            [] { return cc::optional<result_t>(); });
    }

    /// The position of the smallest element, the first of equal ones; empty for an empty sequence.
    [[nodiscard]] constexpr cc::optional<typename impl::seq_dependent<SourceT, isize>::type> index_of_min() &&
    {
        return this->template _index_of_best<true>();
    }
    /// The position of the largest element, the first of equal ones; empty for an empty sequence.
    [[nodiscard]] constexpr cc::optional<typename impl::seq_dependent<SourceT, isize>::type> index_of_max() &&
    {
        return this->template _index_of_best<false>();
    }

    /// The element at position i, by value; 0 <= i < count() must hold.
    /// O(1) where the source knows positions, a walk of i elements otherwise.
    [[nodiscard]] constexpr element_t nth(isize i) &&
    {
        auto r = cc::move(*this).try_nth(i);
        CC_ASSERT(r.has_value(), "nth(i) past the end of the sequence");
        return cc::move(r).value();
    }

    [[nodiscard]] constexpr cc::optional<element_t> try_nth(isize i) &&
    {
        if (i < 0)
            return {};
        if constexpr (impl::seq_has_at<SourceT>)
        {
            if (i >= _source.size())
                return {};
            return cc::optional<element_t>(sequence::_take(_source.at(i)));
        }
        else
            return cc::move(*this).skip(i).try_first();
    }

    //
    // comparing with another sequence
    // (other is a sequence, a container whose values() is taken, or any range)
    //
public:
    /// Whether both have the same elements in the same order.
    template <class OtherT>
    [[nodiscard]] constexpr bool equals(OtherT&& other) &&
    {
        auto b = impl::seq_source_of(cc::forward<OtherT>(other));
        static_assert(!is_infinite || !impl::seq_is_infinite<decltype(b)>, "equals() of two infinite sequences never "
                                                                           "ends");
        if constexpr (has_known_size && impl::seq_has_size<decltype(b)>)
            if (_source.size() != b.size())
                return false;
        if constexpr (impl::seq_has_at<SourceT> && impl::seq_has_at<decltype(b)>)
        {
            auto const n = _source.size();
            for (isize i = 0; i < n; ++i)
                if (!bool(_source.at(i) == b.at(i)))
                    return false;
            return true;
        }
        bool const differs = impl::seq_try_fold(_source,
                                                [&](auto const& x)
                                                {
                                                    auto y = b.next();
                                                    return !y || !bool(x == impl::seq_peek(y));
                                                });
        return !differs && !bool(b.next());
    }

    /// Whether this begins with every element of prefix, in order.
    template <class PrefixT>
    [[nodiscard]] constexpr bool starts_with(PrefixT&& prefix) &&
    {
        auto p = impl::seq_source_of(cc::forward<PrefixT>(prefix));
        static_assert(!impl::seq_is_infinite<decltype(p)>, "starts_with() an infinite prefix never ends");
        if constexpr (impl::seq_has_at<SourceT> && impl::seq_has_at<decltype(p)>)
        {
            auto const n = p.size();
            if (n > _source.size())
                return false;
            for (isize i = 0; i < n; ++i)
                if (!bool(_source.at(i) == p.at(i)))
                    return false;
            return true;
        }
        return !impl::seq_try_fold(p,
                                   [&](auto const& y)
                                   {
                                       auto x = _source.next();
                                       return !x || !bool(impl::seq_peek(x) == y);
                                   });
    }

    /// The position of the first element that differs from other's, or where the shorter ends; empty when equal.
    template <class OtherT>
    [[nodiscard]] constexpr cc::optional<typename impl::seq_dependent<SourceT, isize>::type> mismatch(OtherT&& other) &&
    {
        auto b = impl::seq_source_of(cc::forward<OtherT>(other));
        static_assert(!is_infinite || !impl::seq_is_infinite<decltype(b)>, "mismatch() of two infinite sequences may "
                                                                           "never end");
        if constexpr (impl::seq_has_at<SourceT> && impl::seq_has_at<decltype(b)>)
        {
            auto const na = _source.size();
            auto const nb = b.size();
            auto const n = na < nb ? na : nb;
            for (isize i = 0; i < n; ++i)
                if (!bool(_source.at(i) == b.at(i)))
                    return i;
            if (na != nb)
                return n;
            return {};
        }
        isize idx = 0;
        bool const stopped = impl::seq_try_fold(_source,
                                                [&](auto const& x)
                                                {
                                                    auto y = b.next();
                                                    if (!y || !bool(x == impl::seq_peek(y)))
                                                        return true;
                                                    ++idx;
                                                    return false;
                                                });
        if (stopped || bool(b.next()))
            return idx;
        return {};
    }

    /// The lexicographic order against other, as element <=> element yields it; a proper prefix orders first.
    template <class OtherT>
    [[nodiscard]] constexpr auto compare(OtherT&& other) &&
    {
        auto b = impl::seq_source_of(cc::forward<OtherT>(other));
        static_assert(!is_infinite || !impl::seq_is_infinite<decltype(b)>, "compare() of two infinite sequences may "
                                                                           "never end");
        using other_element_t = typename impl::seq_element_of<typename decltype(b)::item_t>::type;
        static_assert(requires(element_t const& a, other_element_t const& b) { a <=> b; }, "compare() orders by <=>, "
                                                                                           "which these element types "
                                                                                           "do not have");
        using order_t = decltype(std::declval<element_t const&>() <=> std::declval<other_element_t const&>());
        auto result = order_t(isize(0) <=> isize(0));
        bool const stopped = impl::seq_try_fold(_source,
                                                [&](auto const& x)
                                                {
                                                    auto y = b.next();
                                                    if (!y)
                                                    {
                                                        result = order_t(isize(1) <=> isize(0));
                                                        return true;
                                                    }
                                                    auto const c = order_t(x <=> impl::seq_peek(y));
                                                    if (c != 0)
                                                    {
                                                        result = c;
                                                        return true;
                                                    }
                                                    return false;
                                                });
        if (!stopped && bool(b.next()))
            result = order_t(isize(0) <=> isize(1));
        return result;
    }

    /// Whether no element is smaller than the one before it.
    [[nodiscard]] constexpr bool is_sorted() &&
    {
        return cc::move(*this).pairwise().all([](auto const& a, auto const& b) { return !bool(b < a); });
    }

    /// Whether no element's key(idx?, item) is smaller than the key before it; each key is computed once.
    template <class KeyF>
    [[nodiscard]] constexpr bool is_sorted_by(KeyF&& key) &&
    {
        return cc::move(*this).map(cc::forward<KeyF>(key)).is_sorted();
    }

    //
    // reductions to an element's identity
    // (pointers into the elements, nullptr when empty, so they take any sequence with borrowed elements)
    //
public:
    /// A pointer to the smallest element by operator<, the first of equal ones.
    [[nodiscard]] constexpr element_ptr_t min_ptr() && { return this->template _best_ptr<true>(nullptr); }
    /// A pointer to the largest element by operator<, the first of equal ones.
    [[nodiscard]] constexpr element_ptr_t max_ptr() && { return this->template _best_ptr<false>(nullptr); }
    /// A pointer to the element whose key is smallest, the first of equal ones.
    template <class KeyF>
    [[nodiscard]] constexpr element_ptr_t min_ptr_by(KeyF&& key) &&
    {
        element_ptr_t p = nullptr;
        this->template _best_ptr_by<true>(p, key);
        return p;
    }
    /// A pointer to the element whose key is largest, the first of equal ones.
    template <class KeyF>
    [[nodiscard]] constexpr element_ptr_t max_ptr_by(KeyF&& key) &&
    {
        element_ptr_t p = nullptr;
        this->template _best_ptr_by<false>(p, key);
        return p;
    }

    //
    // reductions into an existing accumulator
    // (they compose across several sequences: each call folds one more into the same result)
    //
public:
    /// acc += every element.
    template <class AccT>
    constexpr void sum_into(AccT& acc) &&
    {
        static_assert(!is_infinite, "sum_into() of an infinite sequence never ends: .take(n) first");
        (void)impl::seq_try_fold(_source,
                                 [&](auto const& x)
                                 {
                                     impl::seq_add_to(acc, x);
                                     return false;
                                 });
    }

    /// acc becomes the smallest of itself and every element.
    template <class AccT>
    constexpr void min_into(AccT& acc) &&
    {
        this->template _best_into<true>(acc);
    }
    /// An empty acc takes the first element, then becomes the smallest; an empty sequence leaves it as it was.
    template <class T>
    constexpr void min_into(cc::optional<T>& acc) &&
    {
        this->template _best_into<true>(acc);
    }
    template <class AccT>
    constexpr void max_into(AccT& acc) &&
    {
        this->template _best_into<false>(acc);
    }
    template <class T>
    constexpr void max_into(cc::optional<T>& acc) &&
    {
        this->template _best_into<false>(acc);
    }

    /// p becomes a pointer to the smallest of *p and every element; a null p takes the first.
    constexpr void min_ptr_into(element_ptr_t& p) && { p = this->template _best_ptr<true>(p); }
    constexpr void max_ptr_into(element_ptr_t& p) && { p = this->template _best_ptr<false>(p); }
    /// p becomes a pointer to the element whose key is smallest among *p and every element; a null p takes the first.
    /// The key takes no index, since *p has no position in this sequence.
    template <class KeyF>
    constexpr void min_ptr_by_into(element_ptr_t& p, KeyF&& key) &&
    {
        static_assert(!impl::seq_call_takes_idx<std::remove_reference_t<KeyF>, impl::seq_view_t<item_t>>,
                      "min_ptr_by_into(p, key): the element p points to has no position here, so key takes no index");
        this->template _best_ptr_by<true>(p, key);
    }
    template <class KeyF>
    constexpr void max_ptr_by_into(element_ptr_t& p, KeyF&& key) &&
    {
        static_assert(!impl::seq_call_takes_idx<std::remove_reference_t<KeyF>, impl::seq_view_t<item_t>>,
                      "max_ptr_by_into(p, key): the element p points to has no position here, so key takes no index");
        this->template _best_ptr_by<false>(p, key);
    }

    //
    // non-emptiness
    //
public:
    /// This sequence, known to be non-empty; asserts that it is.
    /// Free where the source can tell it is empty or knows its size, and otherwise buffers the one element it had to pull.
    [[nodiscard]] constexpr auto assert_non_empty() &&
    {
        if constexpr (is_non_empty)
            return cc::move(*this);
        else if constexpr (impl::seq_has_is_empty<SourceT> || impl::seq_has_size<SourceT>)
        {
            CC_ASSERT(!this->_is_empty(), "assert_non_empty() on an empty sequence");
            return sequence<impl::non_empty_source<SourceT>>(impl::non_empty_source<SourceT>(cc::move(_source)));
        }
        else
        {
            auto first = _source.next();
            CC_ASSERT(bool(first), "assert_non_empty() on an empty sequence");
            return sequence<impl::peeked_source<SourceT>>(
                impl::peeked_source<SourceT>(cc::move(first), cc::move(_source)));
        }
    }

    /// This sequence as a non-empty one, or empty if it has no elements.
    ///
    ///   if (auto s = v.values().filter(p).try_as_non_empty(); s.has_value())
    ///       use(cc::move(s.value()).sum());
    [[nodiscard]] constexpr auto try_as_non_empty() &&
    {
        if constexpr (is_non_empty)
            return cc::optional<sequence>(cc::move(*this));
        else if constexpr (impl::seq_has_is_empty<SourceT> || impl::seq_has_size<SourceT>)
        {
            using result_t = sequence<impl::non_empty_source<SourceT>>;
            if (this->_is_empty())
                return cc::optional<result_t>();
            return cc::optional<result_t>(result_t(impl::non_empty_source<SourceT>(cc::move(_source))));
        }
        else
        {
            using result_t = sequence<impl::peeked_source<SourceT>>;
            auto first = _source.next();
            if (!first)
                return cc::optional<result_t>();
            return cc::optional<result_t>(result_t(impl::peeked_source<SourceT>(cc::move(first), cc::move(_source))));
        }
    }

    //
    // transformations
    // (structure-preserving, lazy)
    //
public:
    /// Each element through f(idx?, item); a reference result stays a reference, so map can project members.
    template <class F>
    [[nodiscard]] constexpr auto map(F&& f) &&
    {
        using source = impl::map_source<SourceT, std::decay_t<F>>;
        return sequence<source>(source(cc::move(_source), std::decay_t<F>(cc::forward<F>(f))));
    }

    /// The elements satisfying pred(idx?, item), where idx counts this sequence's elements, not the passing ones.
    template <class P>
    [[nodiscard]] constexpr auto filter(P&& pred) &&
    {
        using source = impl::filter_source<SourceT, std::decay_t<P>>;
        return sequence<source>(source(cc::move(_source), std::decay_t<P>(cc::forward<P>(pred))));
    }

    /// The first n elements; a source that can truncate itself does, so this adds no layer.
    /// A sequence non-empty at compile time may become empty, so the result is no longer typed non-empty.
    [[nodiscard]] constexpr auto take(isize n) &&
    {
        if constexpr (impl::seq_has_truncate<SourceT>)
        {
            _source.truncate(n < 0 ? 0 : n);
            return cc::move(*this)._as_maybe_empty();
        }
        else
            return sequence<impl::take_source<SourceT>>(impl::take_source<SourceT>(cc::move(_source), n));
    }

    /// All but the first n elements; a source that can advance itself skips them unevaluated.
    /// A sequence non-empty at compile time may become empty, so the result is no longer typed non-empty.
    [[nodiscard]] constexpr auto skip(isize n) &&
    {
        if constexpr (impl::seq_has_advance<SourceT>)
        {
            _source.advance(n < 0 ? 0 : n);
            return cc::move(*this)._as_maybe_empty();
        }
        else
            return sequence<impl::skip_source<SourceT>>(impl::skip_source<SourceT>(cc::move(_source), n));
    }

    /// The elements before the first that fails pred(idx?, item).
    template <class P>
    [[nodiscard]] constexpr auto take_while(P&& pred) &&
    {
        using source = impl::take_while_source<SourceT, std::decay_t<P>>;
        return sequence<source>(source(cc::move(_source), std::decay_t<P>(cc::forward<P>(pred))));
    }

    /// The elements from the first that fails pred(idx?, item) on.
    template <class P>
    [[nodiscard]] constexpr auto skip_while(P&& pred) &&
    {
        using source = impl::skip_while_source<SourceT, std::decay_t<P>>;
        return sequence<source>(source(cc::move(_source), std::decay_t<P>(cc::forward<P>(pred))));
    }

    /// This sequence, then other: a sequence, or a container whose values() is taken.
    template <class OtherT>
    [[nodiscard]] constexpr auto chain(OtherT&& other) &&
    {
        auto b = impl::seq_source_of(cc::forward<OtherT>(other));
        using source = impl::chain_source<SourceT, decltype(b)>;
        return sequence<source>(source(cc::move(_source), cc::move(b)));
    }

    /// One element of this and of each other per step, until the shortest ends; callbacks take them spread, f(a, b).
    /// Each other is a sequence, or a container whose values() is taken.
    template <class... OthersT>
    [[nodiscard]] constexpr auto zip(OthersT&&... others) &&
    {
        static_assert(sizeof...(OthersT) > 0, "zip() needs at least one other sequence");
        using source = impl::zip_source<SourceT, decltype(impl::seq_source_of(cc::forward<OthersT>(others)))...>;
        return sequence<source>(source(cc::move(_source), impl::seq_source_of(cc::forward<OthersT>(others))...));
    }

    /// Each element with its position: callbacks take (isize i, item).
    /// It keeps every trait and capability of this sequence: non-empty, infinite, sized, reversible.
    [[nodiscard]] constexpr auto enumerate() &&
    {
        using source = impl::enumerate_source<SourceT>;
        return sequence<source>(source(cc::move(_source)));
    }

    /// The elements of each element, which is a sequence or a container whose values() is taken.
    [[nodiscard]] constexpr auto flatten() &&
    {
        using source = impl::flatten_source<SourceT>;
        return sequence<source>(source(cc::move(_source)));
    }

    /// map(f).flatten(): f returns a sequence or a container per element.
    template <class F>
    [[nodiscard]] constexpr auto flat_map(F&& f) &&
    {
        return cc::move(*this).map(cc::forward<F>(f)).flatten();
    }

    /// Each pair of neighbours, as (a, b): n elements give n - 1 pairs.
    [[nodiscard]] constexpr auto pairwise() && { return cc::move(*this).template windows<2>(); }

    /// Every N consecutive elements, overlapping, as (a, b, ...): n elements give n - N + 1 windows.
    /// Over positions it is an index loop; otherwise it buffers N, and a prvalue window references that buffer.
    template <std::size_t N>
    [[nodiscard]] constexpr auto windows() &&
    {
        using source = impl::window_source<SourceT, N>;
        return sequence<source>(source(cc::move(_source)));
    }

    /// Consecutive groups of N, as (a, b, ...); a trailing group of fewer than N is dropped.
    template <std::size_t N>
    [[nodiscard]] constexpr auto chunks() &&
    {
        using source = impl::chunk_source<SourceT, N>;
        return sequence<source>(source(cc::move(_source)));
    }

    /// Every k-th element, starting with the first; k must be at least 1.
    [[nodiscard]] constexpr auto step_by(isize k) &&
    {
        using source = impl::step_source<SourceT>;
        return sequence<source>(source(cc::move(_source), k));
    }

    /// The elements unchanged, with f(idx?, item) called on each as it passes, for a look into a chain.
    template <class F>
    [[nodiscard]] constexpr auto inspect(F&& f) &&
    {
        using source = impl::inspect_source<SourceT, std::decay_t<F>>;
        return sequence<source>(source(cc::move(_source), std::decay_t<F>(cc::forward<F>(f))));
    }

    /// f(idx?, item) per element, keeping the values it returns: an engaged cc::optional, or a non-null pointer.
    template <class F>
    [[nodiscard]] constexpr auto filter_map(F&& f) &&
    {
        using source = impl::filter_map_source<SourceT, std::decay_t<F>>;
        return sequence<source>(source(cc::move(_source), std::decay_t<F>(cc::forward<F>(f))));
    }

    /// The running fold: acc = f(acc, idx?, item) per element, each acc an element; scan(0, plus) is the prefix sums.
    template <class InitT, class F>
    [[nodiscard]] constexpr auto scan(InitT&& init, F&& f) &&
    {
        using acc_t = std::remove_cvref_t<InitT>;
        using source = impl::scan_source<SourceT, acc_t, std::decay_t<F>>;
        return sequence<source>(
            source(cc::move(_source), acc_t(cc::forward<InitT>(init)), std::decay_t<F>(cc::forward<F>(f))));
    }

    /// Each run of equal consecutive elements, as its first: sorted, it is the distinct elements.
    [[nodiscard]] constexpr auto dedup() &&
    {
        using source = impl::dedup_source<SourceT, impl::seq_identity_key>;
        return sequence<source>(source(cc::move(_source), impl::seq_identity_key{}));
    }

    /// Each run of consecutive elements with equal key(idx?, item), as its first.
    template <class KeyF>
    [[nodiscard]] constexpr auto dedup_by(KeyF&& key) &&
    {
        using source = impl::dedup_source<SourceT, std::decay_t<KeyF>>;
        return sequence<source>(source(cc::move(_source), std::decay_t<KeyF>(cc::forward<KeyF>(key))));
    }

    /// The last n elements; needs a known size.
    [[nodiscard]] constexpr auto take_last(isize n) &&
    {
        static_assert(has_known_size, "take_last() needs a known size to find where the last n begin");
        auto const size = _source.size();
        return cc::move(*this).skip(size - (n < size ? (n < 0 ? 0 : n) : size));
    }

    /// All but the last n elements; needs a known size.
    [[nodiscard]] constexpr auto skip_last(isize n) &&
    {
        static_assert(has_known_size, "skip_last() needs a known size to find where the last n begin");
        auto const size = _source.size();
        return cc::move(*this).take(size - (n < size ? (n < 0 ? 0 : n) : size));
    }

    /// The elements from the last to the first.
    /// Every positional capability mirrors, so reversed().take(n) over a contiguous source is still a pointer range.
    [[nodiscard]] constexpr auto reversed() &&
    {
        static_assert(is_reversible,
                      "reversed() needs a source that steps from the back: contiguous containers, iota, "
                      "repeat(x, n), and map / filter / chain / zip over them; take and skip need a sized "
                      "input, and a callback taking an index needs a known size");
        using source = impl::reverse_source<SourceT>;
        return sequence<source>(source(cc::move(_source)));
    }

    /// The index sequence of this sequence: 0, 1, 2, ... one per element.
    /// With a known size it evaluates nothing; otherwise it counts while consuming the elements.
    [[nodiscard]] constexpr auto indices() &&
    {
        if constexpr (has_known_size)
            return impl::seq_make<is_non_empty>(impl::iota_source(0, _source.size()));
        else
            return cc::move(*this).map([](isize i, auto&&) { return i; });
    }

    //
    // materialization
    // (structure-destroying, terminal)
    //
public:
    /// A new container holding the elements, reserved up front where the size is known.
    /// A sequence owning its container moves the elements out of it.
    template <class ContainerT>
    [[nodiscard]] constexpr ContainerT to_container() &&
    {
        static_assert(sizeof(ContainerT) > 0, "ContainerT must be complete (did you forget to include its header?)");
        static_assert(!impl::seq_is_pack<std::remove_cvref_t<item_t>> || impl::seq_is_complete<element_t>,
                      "a zip materializes as cc::tuple elements: include <clean-core/container/tuple.hh>");

        ContainerT container;
        if constexpr (has_known_size && requires { container.reserve(isize(0)); })
            container.reserve(_source.size());
        cc::move(*this).push_to(container);
        return container;
    }

    /// The elements as a cc::vector; include <clean-core/container/vector.hh> to call it.
    [[nodiscard]] constexpr auto to_vector() &&
    {
        return cc::move(*this).template to_container<cc::vector<element_t>>();
    }

    /// The elements as a cc::array, moved over from the vector they were collected into without a copy.
    /// Include <clean-core/container/array.hh> and <clean-core/container/vector.hh> to call it.
    [[nodiscard]] constexpr auto to_array() &&
    {
        using array_t = cc::array<typename impl::seq_dependent<SourceT, element_t>::type>;
        static_assert(impl::seq_is_complete<array_t>, "to_array() needs <clean-core/container/array.hh>");
        auto v = cc::move(*this).to_vector();
        return array_t::create_from_allocation(v.extract_allocation());
    }

    /// The elements as a cc::fixed_array<element_t, N>; there must be exactly N of them.
    /// Include <clean-core/container/fixed_array.hh> to call it.
    template <isize N>
    [[nodiscard]] constexpr cc::fixed_array<element_t, N> to_fixed_array() &&
    {
        static_assert(impl::seq_is_complete<cc::fixed_array<element_t, N>>, "to_fixed_array() needs "
                                                                            "<clean-core/container/fixed_array.hh>");
        static_assert(std::is_default_constructible_v<element_t>, "to_fixed_array() fills a default-constructed array");
        static_assert(!is_infinite, "materializing an infinite sequence never ends: .take(N) first");
        cc::fixed_array<element_t, N> result;
        isize i = 0;
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     CC_ASSERT(i < N, "to_fixed_array<N>() got more than N elements");
                                     result[i++] = sequence::_take(cc::forward<X>(x));
                                     return false;
                                 });
        CC_ASSERT(i == N, "to_fixed_array<N>() got fewer than N elements");
        return result;
    }

    /// The elements as a cc::vector sorted by compare (default operator<), through cc::sort: not stable.
    /// Include <clean-core/algorithm/sort.hh> and <clean-core/container/vector.hh> to call it.
    template <class... CompareF>
    [[nodiscard]] constexpr auto to_sorted_vector(CompareF&&... compare) &&
    {
        auto v = cc::move(*this).to_vector();
        impl::seq_sort_adl(v, cc::forward<CompareF>(compare)...);
        return v;
    }

    /// The elements as a cc::vector sorted by key(element), through cc::sort_by: not stable.
    template <class KeyF>
    [[nodiscard]] constexpr auto to_sorted_vector_by(KeyF&& key) &&
    {
        auto v = cc::move(*this).to_vector();
        impl::seq_sort_by_adl(v, cc::forward<KeyF>(key));
        return v;
    }

    /// The elements grouped by key(idx?, item) into a cc::map from each key to the elements with it, in sequence order.
    /// The map keeps the keys, so a key must own its value rather than view the element.
    /// Include <clean-core/container/map.hh> and <clean-core/container/vector.hh> to call it.
    template <class KeyF>
    [[nodiscard]] constexpr auto to_grouped_map(KeyF&& key) &&
    {
        using key_t = _key_t<KeyF>;
        using map_t = cc::map<key_t, cc::vector<element_t>>;
        static_assert(impl::seq_is_complete<map_t>, "to_grouped_map() needs <clean-core/container/map.hh>");
        static_assert(!is_infinite, "materializing an infinite sequence never ends: .take(n) first");
        map_t groups;
        auto call = impl::seq_indexed_call<std::remove_reference_t<KeyF>>{key};
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     auto k = key_t(call(impl::seq_view(x)));
                                     groups[k].push_back(sequence::_take(cc::forward<X>(x)));
                                     return false;
                                 });
        return groups;
    }

    /// How many elements each key(idx?, item) has, as a cc::map from key to count; a key must own its value.
    /// Include <clean-core/container/map.hh> to call it.
    template <class KeyF>
    [[nodiscard]] constexpr auto count_by(KeyF&& key) &&
    {
        using key_t = _key_t<KeyF>;
        using map_t = cc::map<key_t, typename impl::seq_dependent<SourceT, isize>::type>;
        static_assert(impl::seq_is_complete<map_t>, "count_by() needs <clean-core/container/map.hh>");
        static_assert(!is_infinite, "materializing an infinite sequence never ends: .take(n) first");
        map_t counts;
        auto call = impl::seq_indexed_call<std::remove_reference_t<KeyF>>{key};
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     ++counts[key_t(call(impl::seq_view(x)))];
                                     return false;
                                 });
        return counts;
    }

    /// The elements that satisfy pred(idx?, item) and those that do not, each in sequence order.
    /// Include <clean-core/container/vector.hh> to call it.
    template <class P>
    [[nodiscard]] constexpr cc::partition_result<element_t> partition(P&& pred) &&
    {
        static_assert(!is_infinite, "materializing an infinite sequence never ends: .take(n) first");
        auto result = cc::partition_result<element_t>();
        auto call = impl::seq_indexed_call<std::remove_reference_t<P>>{pred};
        (void)impl::seq_try_fold(
            _source,
            [&]<class X>(X&& x)
            {
                bool const passes = bool(call(impl::seq_view(x)));
                (passes ? result.passed : result.failed).push_back(sequence::_take(cc::forward<X>(x)));
                return false;
            });
        return result;
    }

    /// A cc::map from key(idx?, item) to value(idx?, item); a later element replaces an earlier one with the same key.
    /// The map keeps both, so neither may view the element.
    /// Include <clean-core/container/map.hh> to call it.
    template <class KeyF, class ValueF>
    [[nodiscard]] constexpr auto to_map(KeyF&& key, ValueF&& value) &&
    {
        using key_t = _key_t<KeyF>;
        using value_t = _key_t<ValueF>;
        using map_t = cc::map<key_t, value_t>;
        static_assert(impl::seq_is_complete<map_t>, "to_map() needs <clean-core/container/map.hh>");
        static_assert(!is_infinite, "materializing an infinite sequence never ends: .take(n) first");
        map_t m;
        auto call_key = impl::seq_indexed_call<std::remove_reference_t<KeyF>>{key};
        auto call_value = impl::seq_indexed_call<std::remove_reference_t<ValueF>>{value};
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     auto k = key_t(call_key(impl::seq_view(x)));
                                     m[cc::move(k)] = value_t(call_value(impl::seq_view(x)));
                                     return false;
                                 });
        return m;
    }

    /// The distinct elements as a cc::set; include <clean-core/container/set.hh> to call it.
    [[nodiscard]] constexpr auto to_set() &&
    {
        using set_t = cc::set<typename impl::seq_dependent<SourceT, element_t>::type>;
        static_assert(impl::seq_is_complete<set_t>, "to_set() needs <clean-core/container/set.hh>");
        static_assert(!is_infinite, "materializing an infinite sequence never ends: .take(n) first");
        set_t result;
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     result.insert(sequence::_take(cc::forward<X>(x)));
                                     return false;
                                 });
        return result;
    }

    /// The elements formatted with "{}" and joined by separator, as a cc::string.
    /// Include <clean-core/string/format.hh> to call it.
    template <class SeparatorT>
    [[nodiscard]] constexpr auto join(SeparatorT const& separator) &&
    {
        using support = impl::seq_format_support<SourceT>;
        static_assert(impl::seq_is_complete<support>, "join() formats its elements: include "
                                                      "<clean-core/string/format.hh>");
        static_assert(!is_infinite, "joining an infinite sequence never ends: .take(n) first");
        return support::join(cc::move(*this), separator);
    }

    /// Appends the elements to an existing container through push_back.
    template <class ContainerT>
    constexpr void push_to(ContainerT& container) &&
    {
        static_assert(!is_infinite, "materializing an infinite sequence never ends: .take(n) first");
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     if constexpr (impl::seq_is_expiring<SourceT> && std::is_lvalue_reference_v<X>)
                                         container.push_back(cc::move(x));
                                     else
                                         container.push_back(impl::seq_as_element(cc::forward<X>(x)));
                                     return false;
                                 });
    }

    //
    // range-for
    // (single-pass: the loop consumes the sequence)
    // A source that is itself a range is iterated directly, which keeps a plain loop over values() a pointer loop.
    // Any other goes through next(), and is external iteration: a hot loop over an adaptor chain wants each() instead.
    //
public:
    struct sentinel
    {
    };

    struct iterator
    {
        SourceT* _source;
        impl::seq_next_t<item_t> _current;

        [[nodiscard]] constexpr decltype(auto) operator*() { return impl::seq_peek(_current); }
        constexpr iterator& operator++()
        {
            _current = _source->next();
            return *this;
        }
        [[nodiscard]] constexpr bool operator!=(sentinel) const { return bool(_current); }
        [[nodiscard]] constexpr bool operator==(sentinel) const { return !bool(_current); }
    };

    [[nodiscard]] constexpr auto begin()
    {
        if constexpr (impl::seq_has_range<SourceT>)
            return _source.begin();
        else
            return iterator{&_source, _source.next()};
    }
    [[nodiscard]] constexpr auto end()
    {
        if constexpr (impl::seq_has_range<SourceT>)
            return _source.end();
        else
            return sentinel{};
    }

    //
    // ctors
    //
public:
    constexpr explicit sequence(SourceT source) : _source(cc::move(source)) {}

    // move-only: a copy would be a second pass over a single-pass source
    sequence(sequence&&) = default;
    sequence& operator=(sequence&&) = default;
    sequence(sequence const&) = delete;
    sequence& operator=(sequence const&) = delete;
    ~sequence() = default;

    //
    // implementation
    //
private:
    SourceT _source;

    // an item as the element it materializes as, moved out where this sequence owns its elements
    template <class X>
    static constexpr element_t _take(X&& x)
    {
        if constexpr (impl::seq_is_expiring<SourceT> && std::is_lvalue_reference_v<X>)
            return element_t(impl::seq_as_element(cc::move(x)));
        else
            return element_t(impl::seq_as_element(cc::forward<X>(x)));
    }

    // whether the source is empty, asked without evaluating an element
    [[nodiscard]] constexpr bool _is_empty() const
    {
        if constexpr (impl::seq_has_is_empty<SourceT>)
            return _source.is_empty();
        else
            return _source.size() == 0;
    }

    // this sequence, no longer typed non-empty, after take or skip reshaped its source
    constexpr auto _as_maybe_empty() &&
    {
        using source_t = typename impl::seq_maybe_empty<SourceT>::type;
        if constexpr (std::is_same_v<source_t, SourceT>)
            return cc::move(*this);
        else
            return sequence<source_t>(source_t(static_cast<SourceT&&>(_source)));
    }

    // state from the first element via init, then step(state, item) over the rest
    // precondition: is_non_empty
    template <class InitF, class StepF>
    constexpr auto _fold_from_first(InitF&& init, StepF&& step)
    {
        static_assert(!is_infinite, "this reduction traverses every element, and the sequence is infinite: .take(n) "
                                    "first");
        auto first = _source.next();
        bool const has_first = bool(first);
        CC_ASSERT(has_first, "a sequence typed non-empty was empty");
        [[assume(has_first)]];
        auto state = init(impl::seq_get(first));
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     step(state, cc::forward<X>(x));
                                     return false;
                                 });
        return state;
    }

    // like _fold_from_first, but on_empty() answers for an empty sequence and on_done(state) for the rest
    template <class InitF, class StepF, class DoneF, class EmptyF>
    constexpr auto _fold_from_first_or(InitF&& init, StepF&& step, DoneF&& on_done, EmptyF&& on_empty)
    {
        static_assert(!is_infinite, "this reduction traverses every element, and the sequence is infinite: .take(n) "
                                    "first");
        if constexpr (is_non_empty)
            return on_done(this->_fold_from_first(init, step));
        else
        {
            auto first = _source.next();
            if (!first)
                return on_empty();
            auto state = init(impl::seq_get(first));
            (void)impl::seq_try_fold(_source,
                                     [&]<class X>(X&& x)
                                     {
                                         step(state, cc::forward<X>(x));
                                         return false;
                                     });
            return on_done(cc::move(state));
        }
    }

    template <class AccT>
    using _average_acc_t
        = std::conditional_t<std::is_void_v<AccT>, typename impl::seq_default_average_acc<element_t>::type, AccT>;
    template <class AccT>
    using _average_t = impl::seq_average<element_t, _average_acc_t<AccT>>;

    // the sum and the count in one pass, then on_done(sum / count); on_empty() for an empty sequence
    template <class AccT, class DoneF, class EmptyF>
    constexpr auto _average(DoneF&& on_done, EmptyF&& on_empty)
    {
        using acc_t = _average_acc_t<AccT>;
        using avg = _average_t<AccT>;
        static_assert(avg::is_defined,
                      "average() sums the elements and divides by the count: the accumulator must take acc + element, "
                      "and divide by its scalar_t where it is not arithmetic");
        isize n = 0;
        isize known_n = 0;
        if constexpr (has_known_size)
            known_n = _source.size();
        return this->_fold_from_first_or(
            [&]<class X>(X&& x)
            {
                ++n;
                return acc_t(cc::forward<X>(x));
            },
            [&](acc_t& acc, auto const& x)
            {
                impl::seq_add_to(acc, x);
                ++n;
            },
            [&](acc_t acc) { return on_done(avg::divide(cc::move(acc), has_known_size ? known_n : n)); }, on_empty);
    }

    // the position of the last element, for a callback walking from the back; only knowable with the size
    template <class F>
    constexpr isize _back_index()
    {
        if constexpr (has_known_size)
            return _source.size() - 1;
        else
        {
            static_assert(!impl::seq_call_takes_idx<std::remove_reference_t<F>, item_t>,
                          "a callback taking an index from the back needs a sequence of known size");
            return 0;
        }
    }

    static constexpr auto _init_min_max()
    {
        return []<class X>(X&& x)
        {
            auto e = sequence::_take(cc::forward<X>(x));
            return cc::min_and_max<element_t>{e, e};
        };
    }
    static constexpr auto _min_max_step()
    {
        return [](cc::min_and_max<element_t>& r, auto const& x)
        {
            if (x < r.min)
                r.min = x;
            else if (r.max < x)
                r.max = x;
        };
    }

    template <bool IsMin>
    constexpr cc::optional<typename impl::seq_dependent<SourceT, isize>::type> _index_of_best()
    {
        static_assert(!is_infinite, "this reduction traverses every element, and the sequence is infinite: .take(n) "
                                    "first");
        auto first = _source.next();
        if (!first)
            return {};
        auto best = sequence::_take(impl::seq_get(first));
        isize best_idx = 0;
        isize idx = 0;
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     ++idx;
                                     if (IsMin ? x < best : best < x)
                                     {
                                         best = sequence::_take(cc::forward<X>(x));
                                         best_idx = idx;
                                     }
                                     return false;
                                 });
        return best_idx;
    }

    // a pointer to the best of *start (when not null) and every element
    template <bool IsMin>
    constexpr element_ptr_t _best_ptr(element_ptr_t start)
    {
        static_assert(has_borrowed_elements, "a pointer reduction needs references that outlive the sequence");
        static_assert(!is_infinite, "this reduction traverses every element, and the sequence is infinite: .take(n) "
                                    "first");
        element_ptr_t best = start;
        (void)impl::seq_try_fold(_source,
                                 [&](item_t x)
                                 {
                                     if (best == nullptr || (IsMin ? x < *best : *best < x))
                                         best = &x;
                                     return false;
                                 });
        return best;
    }

    // p becomes the element with the best key among *p (when not null) and every element; each key is computed once
    template <bool IsMin, class KeyF>
    constexpr void _best_ptr_by(element_ptr_t& p, KeyF& key)
    {
        static_assert(has_borrowed_elements, "a pointer reduction needs references that outlive the sequence");
        static_assert(!is_infinite, "this reduction traverses every element, and the sequence is infinite: .take(n) "
                                    "first");
        using key_t = _key_t<KeyF>;
        cc::optional<key_t> best_key;
        if constexpr (!impl::seq_call_takes_idx<KeyF, impl::seq_view_t<item_t>>)
            if (p != nullptr)
                best_key.emplace_value(cc::invoke(key, impl::seq_view(*p)));
        auto call = impl::seq_indexed_call<KeyF>{key};
        (void)impl::seq_try_fold(_source,
                                 [&](item_t x)
                                 {
                                     auto k = key_t(call(impl::seq_view(x)));
                                     if (p == nullptr || (IsMin ? k < best_key.value() : best_key.value() < k))
                                     {
                                         p = &x;
                                         best_key.emplace_value(cc::move(k));
                                     }
                                     return false;
                                 });
    }

    // acc becomes the best of itself and every element; an empty optional acc takes the first
    template <bool IsMin, class AccT>
    constexpr void _best_into(AccT& acc)
    {
        static_assert(!is_infinite, "this reduction traverses every element, and the sequence is infinite: .take(n) "
                                    "first");
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     if constexpr (requires { acc.has_value(); })
                                     {
                                         if (!acc.has_value() || (IsMin ? x < acc.value() : acc.value() < x))
                                             acc.emplace_value(sequence::_take(cc::forward<X>(x)));
                                     }
                                     else if (IsMin ? x < acc : acc < x)
                                         acc = sequence::_take(cc::forward<X>(x));
                                     return false;
                                 });
    }

    // what key(idx?, item) returns, as the value a reduction keeps
    template <class KeyF>
    using _key_t = std::remove_cvref_t<impl::seq_call_result_t<std::remove_reference_t<KeyF>, impl::seq_view_t<item_t>>>;

    // over references that stay put for the whole fold (borrowed, or owned by this sequence) the best is tracked by
    // pointer and taken once at the end, so a key viewing its element stays valid; otherwise each candidate is taken,
    // and a key must own its value
    template <bool IsMin, class KeyF>
    constexpr element_t _best_by(KeyF& key)
    {
        using key_t = _key_t<KeyF>;
        auto call = impl::seq_indexed_call<KeyF>{key};
        if constexpr (std::is_reference_v<item_t> && (impl::seq_is_borrowed<SourceT> || impl::seq_is_expiring<SourceT>))
        {
            struct best_ref
            {
                element_ptr_t element;
                key_t key;
            };
            auto const make = [&](item_t x) { return best_ref{&x, key_t(call(impl::seq_view(x)))}; };
            auto b = this->_fold_from_first(make,
                                            [&](best_ref& b, item_t x)
                                            {
                                                auto k = key_t(call(impl::seq_view(x)));
                                                if (IsMin ? k < b.key : b.key < k)
                                                {
                                                    b.element = &x;
                                                    b.key = cc::move(k);
                                                }
                                            });
            return sequence::_take(*b.element);
        }
        else
            return this->template _best_by_value<IsMin>(call);
    }

    template <bool IsMin, class CallT>
    constexpr element_t _best_by_value(CallT& call)
    {
        using key_t = std::remove_cvref_t<decltype(call(std::declval<impl::seq_view_t<item_t>>()))>;
        struct best
        {
            element_t element;
            key_t key;
        };
        auto const make = [&]<class X>(X&& x)
        {
            auto k = key_t(call(impl::seq_view(x)));
            return best{sequence::_take(cc::forward<X>(x)), cc::move(k)};
        };
        return this
            ->_fold_from_first(make,
                               [&]<class X>(best& b, X&& x)
                               {
                                   auto candidate = make(cc::forward<X>(x));
                                   if (IsMin ? candidate.key < b.key : b.key < candidate.key)
                                       b = cc::move(candidate);
                               })
            .element;
    }

    static constexpr auto _init_element()
    {
        return []<class X>(X&& x) { return sequence::_take(cc::forward<X>(x)); };
    }
    static constexpr auto _min_step()
    {
        return []<class X>(element_t& m, X&& x)
        {
            if (x < m)
                m = sequence::_take(cc::forward<X>(x));
        };
    }
    static constexpr auto _max_step()
    {
        return []<class X>(element_t& m, X&& x)
        {
            if (m < x)
                m = sequence::_take(cc::forward<X>(x));
        };
    }
    static constexpr auto _last_step()
    {
        return []<class X>(element_t& m, X&& x) { m = sequence::_take(cc::forward<X>(x)); };
    }

    template <class>
    friend struct cc::sequence;
    friend struct impl::seq_access;
};

struct cc::impl::seq_access
{
    template <class S>
    [[nodiscard]] static constexpr S source(cc::sequence<S>&& s)
    {
        return cc::move(s._source);
    }
};

namespace cc
{
/// The integers [begin, end) as a sequence.
[[nodiscard]] constexpr auto iota(isize begin, isize end)
{
    return cc::sequence<impl::iota_source>(impl::iota_source(begin, end));
}

/// The integers [0, end) as a sequence.
[[nodiscard]] constexpr auto iota(isize end)
{
    return cc::iota(0, end);
}

/// value, forever; items are references to the one held copy.
template <class T>
[[nodiscard]] constexpr auto repeat(T value)
{
    using source = impl::repeat_source<std::remove_cvref_t<T>, true>;
    return cc::sequence<source>(source(cc::move(value)));
}

/// value, n times.
template <class T>
[[nodiscard]] constexpr auto repeat(T value, isize n)
{
    using source = impl::repeat_source<std::remove_cvref_t<T>, false>;
    return cc::sequence<source>(source(cc::move(value), n));
}

/// value, once: a sequence non-empty at compile time.
template <class T>
[[nodiscard]] constexpr auto single(T value)
{
    return impl::seq_make<true>(impl::repeat_source<std::remove_cvref_t<T>, false>(cc::move(value), 1));
}

/// No elements of T.
template <class T>
[[nodiscard]] constexpr auto empty_sequence()
{
    return cc::sequence<impl::span_source<T const>>(impl::span_source<T const>(nullptr, nullptr));
}

/// f(), f(), ... forever, each call made when its element is reached.
template <class F>
[[nodiscard]] constexpr auto generate(F&& f)
{
    using source = impl::generate_source<std::decay_t<F>>;
    return cc::sequence<source>(source(std::decay_t<F>(cc::forward<F>(f))));
}

/// init, f(init), f(f(init)), ... forever; f is not called ahead of the element it makes.
template <class T, class F>
[[nodiscard]] constexpr auto iterate(T init, F&& f)
{
    using source = impl::iterate_source<std::remove_cvref_t<T>, std::decay_t<F>>;
    return cc::sequence<source>(source(cc::move(init), std::decay_t<F>(cc::forward<F>(f))));
}

/// Any range as a sequence: a container's own values(), contiguous storage as a span, anything else by begin/end.
/// A temporary is owned, so values_of(make_std_vector()) needs no variable.
template <class RangeT>
[[nodiscard]] constexpr auto values_of(RangeT&& range)
{
    auto s = impl::seq_source_of(cc::forward<RangeT>(range));
    return cc::sequence<decltype(s)>(cc::move(s));
}
/// An initializer list's backing array lives until the end of the full expression, so the sequence must not outlive it:
/// `auto s = cc::values_of({1, 2, 3});` dangles, and no pointer into it escapes (find() does not compile).
template <class T>
[[nodiscard]] constexpr auto values_of(std::initializer_list<T> il)
{
    return cc::sequence<impl::initializer_list_source<T>>(impl::initializer_list_source<T>(il));
}

/// The index sequence 0 .. size of any sized range, a C array included.
template <class RangeT>
[[nodiscard]] constexpr auto indices_of(RangeT const& range)
{
    if constexpr (std::is_array_v<RangeT>)
        return impl::seq_make<(std::extent_v<RangeT> > 0)>(impl::iota_source(0, isize(std::extent_v<RangeT>)));
    else if constexpr (requires { range.size(); })
        return cc::iota(isize(range.size()));
    else
        return cc::iota(isize(range.end() - range.begin()));
}

/// first.zip(others...), where each is a sequence or a container whose values() is taken.
template <class FirstT, class... OthersT>
[[nodiscard]] constexpr auto zip(FirstT&& first, OthersT&&... others)
{
    auto s = impl::seq_source_of(cc::forward<FirstT>(first));
    return cc::sequence<decltype(s)>(cc::move(s)).zip(cc::forward<OthersT>(others)...);
}
} // namespace cc
