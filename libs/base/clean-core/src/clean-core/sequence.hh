#pragma once

#include <clean-core/assert.hh>
#include <clean-core/fwd.hh>
#include <clean-core/optional.hh>
#include <clean-core/utility.hh>

#include <initializer_list>

// NOTE:
// - this header is included by all containers
//   and a lot of other headers
//   so be careful about dependencies here

// -----------------------------------------------------------------------------
// cc::sequence reduction & lifetime model – condensed design summary
// -----------------------------------------------------------------------------

// 1) External iteration (range-for / cursor-next) is the universal fallback.
//    Needed for composition like zip; always available.

// 2) Internal iteration is an *optional optimization protocol*.
//    Reductions dispatch to it when supported; otherwise fall back to external.

// 3) The core internal primitive is an early-outable fold.
//    No wrapper types; control flow is expressed directly in C++.

// 4) fold(init, step) -> fold_outcome
//    - init(idx, first_elem) initializes state from first element
//    - step(idx, state, elem) -> bool (true = early stop)
//    - returns enum { empty, stopped, completed }

// 5) fold_from_first(state, step) -> fold_outcome
//    - assumes state already initialized from first element
//    - used by non-empty adaptors to avoid heterogeneous loops

// 6) fold_outcome separates concerns:
//    - empty      : no elements
//    - stopped    : step returned true
//    - completed  : full traversal, no early-out

// 7) step may or may not accept an index.
//    Index is always available; unused indices are optimized away.

// 8) step may return void (unit) or bool.
//    - void/unit => never early-out
//    - bool      => true triggers early stop
//    Dispatch via if constexpr on return type.
//    -> regular_invoke_with_optional_idx and then unit-vs-bool

// 9) All reductions (min/max/sum/find/any/all/…) are implemented in terms of fold.
//    Only fold itself needs to choose internal vs external iteration.

// 10) Non-empty sequences created at runtime must buffer *exactly one* element.
//     This preserves eval-at-most-once and enables fold_from_first fast paths.

// 11) Reductions on non-empty sequences use fold_from_first.
//     This keeps hot loops branch-free and hand-written-loop-equivalent.

// 12) min/max/sum have two styles:
//     - value-returning (generic, safe-ish)
//     - into-style (updates an existing accumulator)

// 13) *_into APIs update user-provided state.
//     Examples: min_into(T&), min_into(optional<T>&), min_ptr_by_into(T*&).
//     These compose well across multiple ranges.

// 14) Pointer-based *_ptr / *_ptr_by APIs expose element identity.
//     They are only available when elements have stable addresses.

// 15) has_stable_elements trait:
//     - sequence yields references to address-stable storage
//     - propagated conservatively (filter preserves, map-by-value breaks)

// 16) borrowed_elements trait (borrowed_range analogue):
//     - references/pointers remain valid even if the sequence object is temporary
//     - prevents pointers escaping from views over temporary owners

// 17) Pointer-returning APIs require:
//     has_stable_elements && borrowed_elements
//     (strong guard against dangling pointers)

// 18) Reference-returning min() may be allowed with only has_stable_elements.
//     Safe for immediate use; may dangle if the reference escapes the full-expression.
//     This matches span/string_view sharpness.

// 19) Return-type shape-shifting is intentional:
//     - min() returns T& for stable, T for non-stable
//     - auto / auto&& always works; auto& only if truly mutable reference

// 20) Sharp edges are explicit and documented:
//     - underlying container invalidation still applies
//     - trait propagation must be conservative
//     - materialize() exists for users who want ownership & safety

// tri-state result of a try_fold_first operation
enum class sequence_fold_result
{
    // sequence is empty
    empty,
    // step function returned true, fold was stopped
    stopped,
    // step function never returned true, sequence was fully traversed
    completed,
};

// a lazy eval-at-most-once forward cursor abstraction
// with powerful functional compositions
// and predictable performance
// all ops are either
//   transformative / sub sequence
//   into-container-like (to_array/vector, sorted, grouped, ...)
//   statistical (sum, average, min, max, find, count, ...) (fold?)
// propagate some properties
//   has fixed size / bounded size / non-empty
//   has comptime size (max size)
//   is indexable (random access)
// for now
//   NOT copyable, cloneable
//   multi-pass? -> need to create multiple cc::seq
//   makes this all very predictable
// these sequences _can_ be infinite!
//
// important design decisions:
// - each transformation must only add a single template "layer"
// - sequence is the rich-api layer on top with a simple underlying range
// - the sequence must be rich-api but still lightweight enough as a header
//   so that we can let all containers provide sequence members
// - the compiler must easily be able to desugar everything and turn it into basically-optimal assembly
// - we try to include map-like overloads where appropriate (to reduce chaining count)
//
// for authors:
// - RangeT can be a reference (this is encouraged)
//
// ideas:
// - "reversible" range trait
//   all ".last_xyz" are basically just .reversed().xyz()
// - has_stable_elements traits (propagates, means T& and T* are fine, map propagates iff ref-returning)
// - min returns T& for stable, T for non-stable (means auto/auto&/auto const&/auto&& works well)
//
// Internal vs external iteration
//
// Every wrapped range must support *external* iteration (begin/end).
// This is the universal composition model: it chains indefinitely and
// works across arbitrary adaptors (zip, filter, map, …).
//
// However, for many non-trivial ranges (trees, concatenations, flattened
// views, adaptor stacks), the iterator model becomes awkward and often
// inhibits inlining and optimization.
//
// *Internal* iteration flips control flow: the range owns the loop and
// invokes a callback instead of yielding iterators. This matches how such
// data structures are naturally implemented and typically maps much more
// directly to optimal codegen.
//
// Most sequence operations are implemented in terms of internal iteration
// (fold-style). These internal layers compose well and are easier for the
// compiler to inline and collapse than deeply nested iterator machinery.
//
// Rule of thumb:
// if an “internal iteration” is just
//
//     for (auto&& v : *this) callback(v);
//
// then it buys nothing: it is equivalent to external iteration.
// Internal iteration is only worthwhile if the range can do something
// structurally simpler or more direct than the iterator model.
template <class RangeT>
struct cc::sequence
{
private:
    // the underlying range we consume from
    RangeT _range;

    //
    // traits & typedefs
    //
public:
    // value type for the elements
    // e.g. "vector<int>" -> int
    //      "vector<bool> const" -> bool
    using element_t = std::remove_cvref_t<decltype(*cc::begin(_range))>;

    // pointer to the element
    // NOTE: preserves constness
    // e.g. "vector<int>" -> int*
    //      "vector<bool> const" -> bool const*
    using element_ptr_t = std::add_pointer_t<decltype(*cc::begin(_range))>;

    // true iff we can assume that pointers to our elements stay valid until the end of the expression or borrow
    // this is purely a result of the range iterator handing out values or references
    static constexpr bool has_stable_elements = std::is_reference_v<decltype(*cc::begin(_range))>;

    // has_known_size
    // has_known_size_constexpr
    // has_min_size
    // has_min_size_constexpr
    // has_max_size
    // has_max_size_constexpr
    // can_be_infinite
    // can_be_finite (can be asserted to be true in the collectors)
    // is indexable
    // ...
    // can_revisit_elements (repeat, windowed, ...)

    //
    // reductions
    // (structure-consuming, value-producing)
    //
public:
    // fold, sum, min, max, any
    // but also find/count/index_of/first/last

    [[nodiscard]] isize count()
    {
        return this->accumulate( //
            isize(0),            //
            [](isize& cnt, auto&) { ++cnt; });
    }

    [[nodiscard]] isize count_if(auto&& predicate)
    {
        return this->accumulate( //
            isize(0),
            [&predicate](isize idx, isize& cnt, auto& elem)
            {
                if (cc::regular_invoke_with_optional_idx(idx, predicate, elem))
                    ++cnt;
            });
    }

    [[nodiscard]] bool any(auto&& predicate)
    {
        return this->try_fold([&](isize idx, auto& elem)
                              { return bool(cc::regular_invoke_with_optional_idx(idx, predicate, elem)); })
            == sequence_fold_result::stopped; // stopped => we found one with "true", so any is true
    }

    [[nodiscard]] bool all(auto&& predicate)
    {
        return this->try_fold([&](isize idx, auto& elem)
                              { return !bool(cc::regular_invoke_with_optional_idx(idx, predicate, elem)); })
            != sequence_fold_result::stopped; // stopped => we found one with "false", so all is false
    }

    [[nodiscard]] cc::optional<isize> index_of(auto&& predicate)
    {
        cc::optional<isize> result;
        this->try_fold(
            [&](isize idx, auto& elem)
            {
                if (cc::regular_invoke_with_optional_idx(idx, predicate, elem))
                {
                    result = idx;
                    return true; // stop
                }
                else
                    return false;
            });
        return result;
    }

    [[nodiscard]] element_ptr_t find(auto&& predicate)
    {
        static_assert(sequence::has_stable_elements, ".find is only valid if we have stable elements");

        element_t* result = nullptr;
        this->try_fold(
            [&](isize idx, auto& elem)
            {
                if (cc::regular_invoke_with_optional_idx(idx, predicate, elem))
                {
                    result = &elem;
                    return true; // stop
                }
                else
                    return false;
            });
        return result;
    }

    // TODO: name?
    // apply : (idx?, accum&, elem&)
    [[nodiscard]] auto accumulate(auto init, auto&& apply)
    {
        this->try_fold([&](isize idx, auto& elem) { cc::invoke_with_optional_idx(idx, apply, init, elem); });
        return init;
    }

    // calls fun on each element (optional with index first)
    void each(auto&& fun)
    {
        // TODO: preserve value category of elem, aka forward!
        this->try_fold([&](isize idx, auto& elem) { cc::invoke_with_optional_idx(idx, fun, elem); });
    }

    //
    // reductions that need special emptiness handling
    //

    [[nodiscard]] isize sum()
    {
        // TODO: needs non-empty handling
        // return this->accumulate( //
        //     isize(0),            //
        //     [](isize& cnt, auto&) { ++cnt; });
    }

    // min / min_by / min_into / min_by_into / min_ptr / min_or
    // max / ...
    // sum / ...
    // product / ...
    // minmax / ...

    // average / means / ...
    // variance / moment / ...
    // -> whole moment stat package + min + max?

    //
    // transformations
    // (structure-preserving, lazy)
    //
public:
    // map, filter, take, drop, flatten, zip, ...

    // repeat, repeat(isize), ...
    // -> survives indexability, can compute sizes, ...

    //
    // materialization
    // (structure-destroying, terminal)
    // - produce containers (to_vector / to_array / to_container)
    // - write into outputs (push_to / append_to / write_to / collect_into)
    //
public:
    // to_array/vector, collect, append_to, ...

    template <class ContainerT>
    [[nodiscard]] ContainerT to_container()
    {
        static_assert(sizeof(ContainerT) > 0, "ContainerT must be complete (did you forget to include its header?)");

        ContainerT container;
        // TODO: use a container protocol to make "push_back" more generic
        //       and try to reserve
        // TODO: ensure we move if we have an expiring range
        this->each([&]<class T>(T&& elem) { container.push_back(cc::forward<T>(elem)); });
        return container;
    }

    template <class ContainerT>
    [[nodiscard]] ContainerT to_container(auto&& map)
    {
        static_assert(sizeof(ContainerT) > 0, "ContainerT must be complete (did you forget to include its header?)");

        ContainerT container;
        // TODO: use a container protocol to make "push_back" more generic
        //       and try to reserve
        // TODO: ensure we move if we have an expiring range
        this->each([&]<class T>(isize idx, T&& elem)
                   { container.push_back(cc::invoke_with_optional_idx(idx, map, cc::forward<T>(elem))); });
        return container;
    }

    [[nodiscard]] auto to_vector() { return this->to_container<cc::vector<element_t>>(); }
    
    [[nodiscard]] auto to_array()
    {
        // TODO: static assert has_known_size
        return;
    }

    void push_to(auto& container)
    {
        // TODO: use a container protocol to make "push_back" more generic
        //       and try to reserve
        // TODO: ensure we move if we have an expiring range
        this->each([&]<class T>(T&& elem) { container.push_back(cc::forward<T>(elem)); });
    }

    //
    // operational basis
    // i.e. most functions are implemented in terms of these
    //      and we can apply sequence-trait-based optimizations here uniformly
    //
public:
    // the basis of almost all reductions
    // uses init(idx, elem&, bool&) -> State / State& to initialize state
    // then uses step(idx, state&, elem&) -> bool/void to advance
    // if init or step return true, the fold returns with "stopped"
    // if the wrapped range is empty, we return "empty"
    // if we exhausted the range without encountered "true", we return "completed"
    // delegates to range try_fold_first if available
    // otherwise uses external iteration for that
    sequence_fold_result try_fold_first(auto&& init, auto&& step)
    {
        // TODO:
        // if range supports try_fold_first, use that directly

        // fallback based on external iteration
        auto it = cc::begin(_range);
        auto const end = cc::end(_range);

        if (it == end)
            return sequence_fold_result::empty;

        // TODO:
        // do we really want state here?
        // if init & step return bool/void, we avoid weird out params or pair returns
        // I dunno about the codegen quality in every case ...

        bool stop_at_first = false;
        isize idx = 0;
        auto&& state = cc::regular_invoke_with_optional_idx(idx, init, *it, stop_at_first);
        if (stop_at_first)
            return sequence_fold_result::stopped;

        it++;

        while (it != end)
        {
            idx++;

            auto res = cc::regular_invoke_with_optional_idx(idx, step, state, *it);
            if constexpr (!std::is_same_v<decltype(res), cc::unit>)
                if (res)
                    return sequence_fold_result::stopped;

            it++;
        }

        return sequence_fold_result::completed;
    }
    // same as try_fold_first but without init and without state
    // state must be tracked outside basically
    // if step returns true, we early-out of the fold
    sequence_fold_result try_fold(auto&& step)
    {
        // TODO:
        // if range supports try_fold, use that directly

        // fallback based on external iteration
        auto it = cc::begin(_range);
        auto const end = cc::end(_range);

        if (it == end)
            return sequence_fold_result::empty;

        isize idx = 0;

        while (it != end)
        {
            auto res = cc::regular_invoke_with_optional_idx(idx, step, *it);
            if constexpr (!std::is_same_v<decltype(res), cc::unit>)
                if (res)
                    return sequence_fold_result::stopped;

            idx++;
            it++;
        }

        return sequence_fold_result::completed;
    }

    //
    // ctors, fringe api
    //
public:
    explicit sequence(RangeT range) : _range(cc::move(range)) {}

    // non-moveable, non-copyable _for now_
    sequence(sequence&&) = delete;
    sequence(sequence const&) = delete;
    sequence& operator=(sequence&&) = delete;
    sequence& operator=(sequence const&) = delete;
    ~sequence() = default;
};

// factories:
// make_sequence(container)
// make_sequence(init-list)
// make_sequence_from_element(value) -> or other name
// make_sequence_from_generator(...) -> or other name
// (coroutine adapter)
