#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>

#include <initializer_list>

// This header is included by all containers and a lot of other headers, so be careful what it depends on.


/// Tri-state outcome of a try_fold / try_fold_first traversal.
enum class sequence_fold_result
{
    // sequence is empty
    empty,
    // step function returned true, fold was stopped
    stopped,
    // step function never returned true, sequence was fully traversed
    completed,
};

/// A lazy, eval-at-most-once forward cursor over a range, with functional compositions on top.
/// Constructed explicitly from the range, then consumed in place: neither copyable nor movable, so a chain lives as a temporary or a named local.
/// Sequences can be infinite; a second pass needs a second cc::sequence.
///
/// An early prototype.
/// The reductions below work, except sum(); map / filter / take / zip, the factories and to_array() do not exist yet.
/// libs/base/clean-core/docs/sequence.md separates what exists from the design the rest is intended to follow, and is where design decisions belong.
///
/// RangeT can be a reference, and that is encouraged.
template <class RangeT>
struct cc::sequence
{
private:
    RangeT _range;

    //
    // traits & typedefs
    //
public:
    // Element value type: "vector<int>" -> int, "vector<bool> const" -> bool.
    using element_t = std::remove_cvref_t<decltype(*cc::begin(_range))>;

    // Pointer to the element, preserving constness: "vector<int>" -> int*, "vector<bool> const" -> bool const*.
    using element_ptr_t = std::add_pointer_t<decltype(*cc::begin(_range))>;

    // True iff pointers to our elements stay valid until the end of the expression or borrow.
    // Purely a consequence of whether the range's iterator hands out references or values.
    static constexpr bool has_stable_elements = std::is_reference_v<decltype(*cc::begin(_range))>;


    //
    // reductions
    // (structure-consuming, value-producing)
    //
public:
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

    // Calls fun on each element, with the index passed first if it accepts one.
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
        // TODO: not implemented — needs non-empty handling, and calling this today falls off the end of a non-void function.
    }


    //
    // transformations
    // (structure-preserving, lazy)
    //
public:
    //
    // materialization
    // (structure-destroying, terminal)
    // - produce containers (to_vector / to_array / to_container)
    // - write into outputs (push_to / append_to / write_to / collect_into)
    //
public:
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

    /// TODO: not implemented — returns nothing today.
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
    // The intended basis of almost all reductions, though every reduction here currently goes through try_fold instead.
    // init(idx, elem&, bool&) -> State / State& initializes the state from the first element, then step(idx, state&, elem&) -> bool/void advances it.
    // Returns "stopped" if init or step returned true, "empty" if the wrapped range had no elements, and "completed" if the range was exhausted without one.
    // Meant to delegate to the range's own try_fold_first where it has one; today it always uses external iteration.
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
    // try_fold_first without the init step and without state, so state must be tracked by the caller.
    // step returning true early-outs of the fold.
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

// Factories (make_sequence and friends) and a coroutine adapter are still open — see libs/base/clean-core/docs/sequence.md.
