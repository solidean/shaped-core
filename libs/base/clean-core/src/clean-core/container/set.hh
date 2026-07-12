#pragma once

#include <clean-core/common/utility.hh> // cc::unit — the empty stand-in value
#include <clean-core/container/map.hh>
#include <clean-core/fwd.hh>

/// Hash set of unique T using separate chaining — a cc::map<T, unit> underneath, where the empty value is
/// free (the node stores it with [[no_unique_address]]). Inherits the map's properties:
///   - T may be immovable and/or non-copyable; elements are heap-allocated and never move.
///   - Element references stay valid across unrelated insertions/erasures and across growth.
///   - Heterogeneous membership tests and inserts: contains/erase/insert take any probe type the hasher and
///     comparator accept.
///
/// Not thread-safe for mutation; concurrent readers are fine. See cc::map for the hashing requirements.
template <class T, class Hash, class KeyEqual>
struct cc::set
{
    // the empty unit value costs nothing thanks to [[no_unique_address]] on the map node's value
    using map_t = cc::map<T, cc::unit, Hash, KeyEqual>;

    // iterator (yields T const& — set elements are immutable)
public:
    struct iterator
    {
        typename map_t::const_iterator it;

        [[nodiscard]] T const& operator*() const { return (*it).key; }
        iterator& operator++()
        {
            ++it;
            return *this;
        }
        [[nodiscard]] bool operator==(iterator const& rhs) const { return it == rhs.it; }
        [[nodiscard]] bool operator!=(iterator const& rhs) const { return it != rhs.it; }
    };

    // ctors
public:
    set() = default;
    explicit set(cc::node_memory_resource* node_resource) : _map(node_resource) {}

    // queries
public:
    [[nodiscard]] isize size() const { return _map.size(); }
    [[nodiscard]] bool empty() const { return _map.empty(); }
    [[nodiscard]] isize bucket_count() const { return _map.bucket_count(); }

    template <class K2>
    [[nodiscard]] bool contains(K2 const& k) const
    {
        return _map.contains(k);
    }

    // mutation
public:
    /// Insert an element built from `key` if not already present; returns whether a new element was added.
    /// T is constructed from `key`, so with a heterogeneous key type this doubles as an emplace.
    template <class K2>
    bool insert(K2 const& key)
    {
        auto e = _map.entry(key);
        bool const is_new = !e.exists();
        if (is_new)
            e.emplace();
        return is_new;
    }

    template <class K2>
    bool erase(K2 const& k)
    {
        return _map.erase(k);
    }

    void clear() { _map.clear(); }
    void reserve(isize n) { _map.reserve(n); }

    // iteration
public:
    [[nodiscard]] iterator begin() const { return iterator{_map.begin()}; }
    [[nodiscard]] iterator end() const { return iterator{_map.end()}; }

    // members
private:
    map_t _map;
};
