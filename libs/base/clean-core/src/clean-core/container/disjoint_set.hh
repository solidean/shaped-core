#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::swap
#include <clean-core/container/vector.hh>

#include <type_traits>

/// Union-find over elements numbered 0 .. element_count()-1, each belonging to exactly one set.
///
/// A set is named by its representative, an arbitrary one of its members; two elements are in the same set iff they
/// reach the same representative.
/// In the literature 'find' is get_representative and 'union' is merge_by_element / merge_by_representative.
///
/// There is deliberately no size(): it would not say whether it counts elements or sets.
/// The two counts are element_count() and partition_count(), and every other name says which of the two it means.
///
/// `IdxT` is the stored index type, so an element costs 8 bytes at `i32` and 4 at `i16`.
/// Growing past what IdxT can address asserts.
///
/// GOTCHA: get_representative and everything reaching it are non-const — a find rewires the parent links it walked.
/// The const observers are the ones that answer from a single link: get_parent, is_representative and
/// size_of_set_by_representative.
///
/// Union by size plus path halving, which is the O(alpha(n)) amortized pair: a find rewrites every second link on its
/// way up in the same single pass, without recursion and without a second walk down.
///
/// See [containers](../../../docs/containers.md) for the contracts every container shares.
///
/// Usage:
///   auto ds = cc::disjoint_set<i32>::create_singletons(vertex_count);
///   for (auto const& e : edges)
///       ds.merge_by_element(e.from, e.to);
///   auto const [component_to_representative, element_to_component] = ds.compute_components();
template <class IdxT>
struct cc::disjoint_set
{
    static_assert(std::is_integral_v<IdxT> && !std::is_same_v<IdxT, bool>, "disjoint_set indices must be integers");

    // queries
public:
    /// Number of elements, which are numbered 0 .. element_count()-1.
    [[nodiscard]] isize element_count() const { return _entries.size(); }

    /// Number of disjoint sets; starts at element_count() and drops by one per successful merge.
    [[nodiscard]] isize partition_count() const { return _partition_count; }

    /// Elements that fit without reallocation.
    [[nodiscard]] isize capacity() const { return _entries.capacity(); }

    /// The representative of the set `e` belongs to, compressing the path it walked.
    [[nodiscard]] IdxT get_representative(IdxT e)
    {
        CC_ASSERT(0 <= isize(e) && isize(e) < element_count(), "element index out of range");

        auto* const entries = _entries.data();
        while (entries[e].parent != e)
        {
            entries[e].parent = entries[entries[e].parent].parent; // path halving: skip every second link
            e = entries[e].parent;
        }
        return e;
    }

    /// The parent link of `e`, one step only, and `e` itself when it is the representative.
    /// This is neither the representative nor a compressing read.
    [[nodiscard]] IdxT get_parent(IdxT e) const
    {
        CC_ASSERT(0 <= isize(e) && isize(e) < element_count(), "element index out of range");
        return _entries[e].parent;
    }

    /// True iff `e` is the representative of its set.
    [[nodiscard]] bool is_representative(IdxT e) const { return get_parent(e) == e; }

    [[nodiscard]] bool are_in_same_set(IdxT e0, IdxT e1) { return get_representative(e0) == get_representative(e1); }

    /// Number of elements in the set `e` belongs to.
    [[nodiscard]] IdxT size_of_set_by_element(IdxT e) { return _entries[get_representative(e)].set_size; }

    /// Number of elements in the set `e_repr` represents.
    /// `e_repr` must be a representative.
    [[nodiscard]] IdxT size_of_set_by_representative(IdxT e_repr) const
    {
        CC_ASSERT(is_representative(e_repr), "not a representative element");
        return _entries[e_repr].set_size;
    }

    // merging
public:
    /// Merges the sets of two arbitrary elements.
    /// Returns true iff they were in different sets, i.e. iff anything was merged.
    bool merge_by_element(IdxT e0, IdxT e1)
    {
        return merge_by_representative(get_representative(e0), get_representative(e1));
    }

    /// Merges the sets of two representatives.
    /// Both arguments must be representatives; returns true iff they differed.
    bool merge_by_representative(IdxT e0_repr, IdxT e1_repr)
    {
        CC_ASSERT(is_representative(e0_repr), "not a representative element");
        CC_ASSERT(is_representative(e1_repr), "not a representative element");

        if (e0_repr == e1_repr)
            return false;

        auto* const entries = _entries.data();

        // union by size, so the shallower tree is the one that gains a level
        if (entries[e0_repr].set_size < entries[e1_repr].set_size)
            cc::swap(e0_repr, e1_repr);

        entries[e1_repr].parent = e0_repr;
        entries[e0_repr].set_size += entries[e1_repr].set_size;
        --_partition_count;

        return true;
    }

    // components
public:
    /// Both halves of compute_components, for call sites that do not reuse their storage.
    struct components
    {
        /// component index -> the representative element of that set.
        cc::vector<IdxT> component_to_representative;

        /// element index -> the component index of the set it belongs to.
        cc::vector<IdxT> element_to_component;

        [[nodiscard]] isize component_count() const { return component_to_representative.size(); }
    };

    /// Numbers the sets 0 .. partition_count()-1 and writes both directions of the numbering into the given vectors.
    ///
    /// Both are overwritten rather than appended to, and both keep whatever capacity they already had, so a caller
    /// that reuses its vectors across calls allocates nothing.
    /// The component indices ascend in the representatives' element order, so the numbering is deterministic for a
    /// given partitioning however it was built.
    ///
    /// Compresses every path, like any other find.
    void compute_components(cc::vector<IdxT>& out_component_to_representative, cc::vector<IdxT>& out_element_to_component)
    {
        auto const n = element_count();
        out_element_to_component.clear_resize_to_filled(n, IdxT(-1));
        out_component_to_representative.clear();
        out_component_to_representative.reserve(_partition_count);

        auto* const entries = _entries.data();

        for (isize e = 0; e < n; ++e)
            if (entries[e].parent == IdxT(e))
            {
                out_element_to_component[e] = IdxT(out_component_to_representative.size());
                out_component_to_representative.push_back_stable(IdxT(e));
            }

        for (isize e = 0; e < n; ++e)
            if (entries[e].parent != IdxT(e))
                out_element_to_component[e] = out_element_to_component[get_representative(IdxT(e))];
    }

    /// compute_components into two fresh vectors.
    [[nodiscard]] components compute_components()
    {
        auto r = components();
        compute_components(r.component_to_representative, r.element_to_component);
        return r;
    }

    // growth
public:
    /// Appends one singleton element and returns its index.
    IdxT add_element() { return add_elements(1); }

    /// Appends `count` singleton elements and returns the index of the first one.
    IdxT add_elements(isize count)
    {
        CC_ASSERT(count >= 0, "element count must be non-negative");
        auto const first = element_count();
        CC_ASSERT(first + count <= _max_element_count, "element count exceeds what the index type can address");

        _entries.reserve(first + count);
        for (isize i = first; i < first + count; ++i)
            _entries.push_back_stable({IdxT(i), IdxT(1)});

        _partition_count += count;
        return IdxT(first);
    }

    /// Appends every element of `rhs` after ours, keeping its partitioning, and returns the index its element 0 landed at.
    /// Nothing merges across the seam: an appended element shares a set with exactly the elements it already shared one with.
    /// Appending a set to itself is allowed and duplicates its partitioning.
    IdxT append(disjoint_set const& rhs)
    {
        auto const offset = element_count();
        auto const rhs_count = rhs.element_count();
        CC_ASSERT(offset + rhs_count <= _max_element_count, "element count exceeds what the index type can address");

        // reserve before reading rhs, so a self-append reads the surviving allocation
        _entries.reserve(offset + rhs_count);
        auto const* const rhs_entries = rhs._entries.data();
        for (isize i = 0; i < rhs_count; ++i)
            _entries.push_back_stable({IdxT(rhs_entries[i].parent + offset), rhs_entries[i].set_size});

        _partition_count += rhs._partition_count;
        return IdxT(offset);
    }

    /// Ensures `count` elements fit without reallocation.
    void reserve(isize count)
    {
        CC_ASSERT(count <= _max_element_count, "element count exceeds what the index type can address");
        _entries.reserve(count);
    }

    /// Drops every element, so element_count() becomes 0; the capacity survives.
    void clear()
    {
        _entries.clear();
        _partition_count = 0;
    }

    /// Discards the current content and restarts with `count` singleton elements.
    void reset_to_singletons(isize count)
    {
        clear();
        add_elements(count);
    }

    // factories
public:
    /// `count` elements, each its own singleton set.
    [[nodiscard]] static disjoint_set create_singletons(isize count, cc::memory_resource const* resource = nullptr)
    {
        auto r = create_with_resource(resource);
        r.add_elements(count);
        return r;
    }

    /// No elements, but storage for `count` of them.
    [[nodiscard]] static disjoint_set create_with_capacity(isize count, cc::memory_resource const* resource = nullptr)
    {
        auto r = create_with_resource(resource);
        r.reserve(count);
        return r;
    }

    /// Empty, and allocating from `resource` from here on.
    [[nodiscard]] static disjoint_set create_with_resource(cc::memory_resource const* resource)
    {
        auto r = disjoint_set();
        r._entries = cc::vector<entry>::create_with_resource(resource);
        return r;
    }

    // implementation
private:
    /// One element's node: its parent link, plus the set size that is only meaningful while it is a representative.
    struct entry
    {
        IdxT parent;
        IdxT set_size;
    };

    /// Bits IdxT has available for a non-negative index.
    static constexpr isize _index_bits = isize(sizeof(IdxT)) * 8 - (std::is_signed_v<IdxT> ? 1 : 0);

    /// Elements IdxT can address, clamped to what isize itself can count.
    static constexpr isize _max_element_count = _index_bits >= 63 ? isize(~(u64(1) << 63)) : isize(u64(1) << _index_bits);

    cc::vector<entry> _entries;
    isize _partition_count = 0;
};
