#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>
#include <clean-core/sequence/impl/adaptors.hh>
#include <clean-core/sequence/source.hh>

// Every container reaches this header, so be careful what it depends on.

/// A lazy, single-pass, eval-at-most-once sequence over a source, with functional compositions on top.
/// Containers hand one out through values() and indices(); see libs/base/clean-core/docs/sequence.md.
///
///   auto const n = v.values().filter([](int x) { return x > 0; }).count();
///   auto const s = v.values().assert_non_empty().sum();
///
/// A sequence references the container it came from and must not outlive it; it is a temporary, like span.
/// Terminal operations are &&-qualified and consume it, so `cc::move(s).count()` is how a named one is spent.
/// A callback may take a leading `isize` index, its item's position in the sequence it is applied to.
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

    // the item decayed: "int&" -> int, "bool const&" -> bool
    using element_t = std::remove_cvref_t<item_t>;

    // pointer to an item, constness preserved; only meaningful with stable elements
    using element_ptr_t = std::add_pointer_t<std::remove_reference_t<item_t>>;

    // at least one element, known at compile time
    static constexpr bool is_non_empty = impl::seq_is_non_empty<SourceT>;

    // items are references to address-stable storage
    static constexpr bool has_stable_elements = std::is_reference_v<item_t>;

    // those references stay valid after this sequence dies, so a pointer may escape it
    static constexpr bool has_borrowed_elements = has_stable_elements && impl::seq_is_borrowed<SourceT>;

    // the element count is available without traversal
    static constexpr bool has_known_size = impl::seq_has_size<SourceT>;

    //
    // reductions
    // (structure-consuming, value-producing)
    //
public:
    [[nodiscard]] constexpr isize count() &&
    {
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
        auto call = impl::seq_indexed_call<std::remove_reference_t<decltype(f)>>{f};
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     (void)call(cc::forward<X>(x));
                                     return false;
                                 });
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

    /// init plus the sum of all elements; init's type is the accumulator, so it must hold acc + x without conversion.
    template <class InitT>
    [[nodiscard]] constexpr std::remove_cvref_t<InitT> sum_from(InitT&& init) &&
    {
        using acc_t = std::remove_cvref_t<InitT>;
        static_assert(
            std::is_same_v<std::remove_cvref_t<decltype(std::declval<acc_t const&>() + std::declval<element_t const&>())>, acc_t>,
            "sum_from(init): acc + element must have init's type, or every step converts it "
            "(sum_from(0) over floats truncates); start from a value of the type the sum should have");
        acc_t acc = cc::forward<InitT>(init);
        (void)impl::seq_try_fold(_source,
                                 [&](auto const& x)
                                 {
                                     impl::seq_add_to(acc, x);
                                     return false;
                                 });
        return acc;
    }

    /// The smallest element by operator<, the first of equal ones, by value.
    [[nodiscard]] constexpr element_t min() &&
    {
        static_assert(is_non_empty, "min() needs a non-empty sequence: spell .assert_non_empty().min(), branch on "
                                    ".try_as_non_empty(), or use .min_or(fallback) / .try_min()");
        return this->_fold_from_first(_init_element(), _min_step());
    }

    /// The largest element by operator<, the first of equal ones, by value.
    [[nodiscard]] constexpr element_t max() &&
    {
        static_assert(is_non_empty, "max() needs a non-empty sequence: spell .assert_non_empty().max(), branch on "
                                    ".try_as_non_empty(), or use .max_or(fallback) / .try_max()");
        return this->_fold_from_first(_init_element(), _max_step());
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

    //
    // non-emptiness
    //
public:
    /// This sequence, known to be non-empty; asserts that it is.
    /// Free where the source can tell it is empty, and otherwise buffers the one element it had to pull to find out.
    [[nodiscard]] constexpr auto assert_non_empty() &&
    {
        if constexpr (is_non_empty)
            return cc::move(*this);
        else if constexpr (impl::seq_has_is_empty<SourceT>)
        {
            CC_ASSERT(!_source.is_empty(), "assert_non_empty() on an empty sequence");
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
    ///   if (auto s = v.values().filter(p).try_as_non_empty())
    ///       use(cc::move(s.value()).sum());
    [[nodiscard]] constexpr auto try_as_non_empty() &&
    {
        if constexpr (is_non_empty)
            return cc::optional<sequence>(cc::move(*this));
        else if constexpr (impl::seq_has_is_empty<SourceT>)
        {
            using result_t = sequence<impl::non_empty_source<SourceT>>;
            if (_source.is_empty())
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

    /// The index sequence of this sequence: 0, 1, 2, ... one per element.
    /// With a known size it evaluates nothing; otherwise it counts while consuming the elements.
    [[nodiscard]] constexpr auto indices() &&
    {
        if constexpr (has_known_size)
        {
            auto source = impl::iota_source(0, _source.size());
            if constexpr (is_non_empty)
                return sequence<impl::non_empty_source<impl::iota_source>>(
                    impl::non_empty_source<impl::iota_source>(cc::move(source)));
            else
                return sequence<impl::iota_source>(source);
        }
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

    /// Appends the elements to an existing container through push_back.
    template <class ContainerT>
    constexpr void push_to(ContainerT& container) &&
    {
        (void)impl::seq_try_fold(_source,
                                 [&]<class X>(X&& x)
                                 {
                                     if constexpr (impl::seq_is_expiring<SourceT> && std::is_lvalue_reference_v<X>)
                                         container.push_back(cc::move(x));
                                     else
                                         container.push_back(cc::forward<X>(x));
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

    // state from the first element via init, then step(state, item) over the rest
    // precondition: is_non_empty
    template <class InitF, class StepF>
    constexpr auto _fold_from_first(InitF&& init, StepF&& step)
    {
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

    static constexpr auto _init_element()
    {
        return []<class X>(X&& x) { return element_t(cc::forward<X>(x)); };
    }
    static constexpr auto _min_step()
    {
        return []<class X>(element_t& m, X&& x)
        {
            if (x < m)
                m = cc::forward<X>(x);
        };
    }
    static constexpr auto _max_step()
    {
        return []<class X>(element_t& m, X&& x)
        {
            if (m < x)
                m = cc::forward<X>(x);
        };
    }

    template <class>
    friend struct cc::sequence;
};
