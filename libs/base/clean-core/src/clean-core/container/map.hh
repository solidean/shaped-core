#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/memory/node_allocation.hh>

#include <type_traits>

// default_hash lives in common/hash.hh, default_equal in common/utility.hh (shared by cc::map and cc::set).

/// Hash map from K to V using separate chaining, with power-of-two bucket counts.
///
/// Chosen properties (the reason this is not a flat/open-addressing table):
///   - K and V may be immovable and/or non-copyable — nodes are heap-allocated and never move.
///   - References (K&, V&) stay valid across unrelated insertions and erasures, and across growth: a
///     rehash only relinks node pointers, it never touches the nodes.
///   - Heterogeneous lookup: get/contains/entry take any probe type the hasher and comparator accept.
///
/// Hashing must be well-mixed because only the low bits index the bucket array. The default hasher
/// finalizes for you; a custom Hash must avalanche and must agree across K and any probe type.
///
/// The map is not thread-safe for mutation, but concurrent readers are fine (entries are self-contained
/// and stash nothing in the map). Any structural mutation invalidates outstanding entries and iterators.
template <class K, class V, class Hash, class KeyEqual>
struct cc::map
{
    static_assert(std::is_object_v<K> && !std::is_const_v<K>, "map key must be a non-const object type");
    static_assert(std::is_object_v<V> && !std::is_const_v<V>, "map value must be a non-const object type");

    // node
private:
    struct node
    {
        u64 hash;   // finalized hash, kept to skip rehashing K on growth and to short-circuit chain compares
        node* next; // singly-linked collision chain (insert-at-head)
        K key;
        [[no_unique_address]] V value; // no_unique_address makes an empty V free — cc::set rides on this

        // constructs key from a single arg and value from the rest, both in place (immovable-safe)
        template <class KArg, class... VArgs>
        explicit node(u64 h, node* nx, KArg&& karg, VArgs&&... vargs)
          : hash(h), next(nx), key(cc::forward<KArg>(karg)), value(cc::forward<VArgs>(vargs)...)
        {
        }
    };

    // iterator
public:
    template <bool IsConst>
    struct iterator_t
    {
        using map_ptr = std::conditional_t<IsConst, map const*, map*>;

        /// What iteration yields: references into the live node, so it supports `for (auto [k, v] : m)`.
        struct reference
        {
            K const& key;
            std::conditional_t<IsConst, V const, V>& value;
        };

        map_ptr owner = nullptr;
        isize bucket = 0;
        node* current = nullptr;

        [[nodiscard]] reference operator*() const { return reference{current->key, current->value}; }

        iterator_t& operator++()
        {
            current = current->next;
            advance_to_valid();
            return *this;
        }

        // from (bucket, current); if current is null, walks forward to the next non-empty bucket
        void advance_to_valid()
        {
            auto const b = owner->_buckets.obj_span();
            while (current == nullptr)
            {
                ++bucket;
                if (bucket >= b.size())
                    return; // reached end (bucket == bucket_count, current == null)
                current = b[bucket];
            }
        }

        [[nodiscard]] bool operator==(iterator_t const& rhs) const
        {
            return current == rhs.current && bucket == rhs.bucket;
        }
        [[nodiscard]] bool operator!=(iterator_t const& rhs) const { return !(*this == rhs); }
    };
    using iterator = iterator_t<false>;
    using const_iterator = iterator_t<true>;

    // entry
public:
    /// Mutable lookup-and-maybe-insert handle over one key slot (Rust-inspired). Self-contained: caches the
    /// hash, a copy of the probe key, and the found node (if any), storing nothing in the map — so the
    /// handle stays valid even if the caller's key was a temporary.
    ///
    /// Invalidated by ANY structural mutation of the owning map (insert/erase/rehash, including via another
    /// entry's emplace). Do not mutate the map between entry() and emplace().
    template <class K2>
    struct entry_handle
    {
        // constructed by map::entry with a copy of the probe key (node is private, so this is internal)
        entry_handle(map* owner, K2 probe, u64 hash, node* found)
          : _owner(owner), _probe(cc::move(probe)), _hash(hash), _found(found)
        {
        }

        [[nodiscard]] bool exists() const { return _found != nullptr; }
        explicit operator bool() const { return _found != nullptr; }

        [[nodiscard]] K const& key() const
        {
            CC_ASSERT(_found != nullptr, "entry is vacant");
            return _found->key;
        }
        [[nodiscard]] V& value() const
        {
            CC_ASSERT(_found != nullptr, "entry is vacant");
            return _found->value;
        }

        /// Insert on the vacant path: K is constructed from the probe key, V from vargs. Must be vacant.
        template <class... VArgs>
        V& emplace(VArgs&&... vargs)
        {
            CC_ASSERT(_found == nullptr, "entry is already occupied");
            _found = _owner->insert_node(_hash, cc::move(_probe), cc::forward<VArgs>(vargs)...);
            return _found->value;
        }

        /// Like emplace, but builds K from an explicit key instead of the probe key (divergent construction).
        template <class KArg, class... VArgs>
        V& emplace_with_key(KArg&& karg, VArgs&&... vargs)
        {
            CC_ASSERT(_found == nullptr, "entry is already occupied");
            _found = _owner->insert_node(_hash, cc::forward<KArg>(karg), cc::forward<VArgs>(vargs)...);
            return _found->value;
        }

        /// Return the existing value, or insert (K from probe key, V from vargs) and return that.
        template <class... VArgs>
        V& get_or_emplace(VArgs&&... vargs)
        {
            if (_found == nullptr)
                _found = _owner->insert_node(_hash, cc::move(_probe), cc::forward<VArgs>(vargs)...);
            return _found->value;
        }

    private:
        map* _owner;
        K2 _probe;
        u64 _hash;
        node* _found;
    };

    /// Read-only entry handle over one key slot (from a const map): lookup only, no insertion.
    template <class K2>
    struct const_entry_handle
    {
        explicit const_entry_handle(node* found) : _found(found) {}

        [[nodiscard]] bool exists() const { return _found != nullptr; }
        explicit operator bool() const { return _found != nullptr; }

        [[nodiscard]] K const& key() const
        {
            CC_ASSERT(_found != nullptr, "entry is vacant");
            return _found->key;
        }
        [[nodiscard]] V const& value() const
        {
            CC_ASSERT(_found != nullptr, "entry is vacant");
            return _found->value;
        }

    private:
        node* _found;
    };

    // ctors / dtor
public:
    map() = default;
    explicit map(cc::node_memory_resource* node_resource) : _node_res(node_resource) {}

    map(map&& rhs) noexcept
      : _buckets(cc::move(rhs._buckets)),
        _size(cc::exchange(rhs._size, 0)),
        _node_res(rhs._node_res),
        _hasher(rhs._hasher),
        _eq(rhs._eq)
    {
    }
    map& operator=(map&& rhs) noexcept
    {
        if (this != &rhs)
        {
            destroy_all_nodes();
            _buckets = cc::move(rhs._buckets); // frees our old bucket array, steals rhs's
            _size = cc::exchange(rhs._size, 0);
            _node_res = rhs._node_res;
            _hasher = rhs._hasher;
            _eq = rhs._eq;
        }
        return *this;
    }

    // deep copy, enabled only when both K and V are copyable (immovable elements make the map move-only)
    map(map const& rhs)
        requires(std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V>)
      : _node_res(rhs._node_res), _hasher(rhs._hasher), _eq(rhs._eq)
    {
        copy_from(rhs);
    }
    map& operator=(map const& rhs)
        requires(std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V>)
    {
        map tmp(rhs);
        *this = cc::move(tmp);
        return *this;
    }

    ~map() { destroy_all_nodes(); }

    // queries
public:
    [[nodiscard]] isize size() const { return _size; }
    [[nodiscard]] bool empty() const { return _size == 0; }

    /// Number of buckets (a power of two, or zero for an untouched map).
    [[nodiscard]] isize bucket_count() const { return _buckets.obj_span().size(); }

    // lookup (heterogeneous, no construction)
public:
    /// Reference to the value for k; k must be present.
    template <class K2>
    [[nodiscard]] V& get(K2 const& k)
    {
        node* n = find_node(k, _hasher(k));
        CC_ASSERT(n != nullptr, "key not found");
        return n->value;
    }
    template <class K2>
    [[nodiscard]] V const& get(K2 const& k) const
    {
        node* n = find_node(k, _hasher(k));
        CC_ASSERT(n != nullptr, "key not found");
        return n->value;
    }

    /// Pointer to the value for k, or nullptr if absent.
    template <class K2>
    [[nodiscard]] V* get_ptr(K2 const& k)
    {
        node* n = find_node(k, _hasher(k));
        return n != nullptr ? &n->value : nullptr;
    }
    template <class K2>
    [[nodiscard]] V const* get_ptr(K2 const& k) const
    {
        node* n = find_node(k, _hasher(k));
        return n != nullptr ? &n->value : nullptr;
    }

    template <class K2>
    [[nodiscard]] bool contains(K2 const& k) const
    {
        return find_node(k, _hasher(k)) != nullptr;
    }

    /// Copy the value for k into out and return true, or leave out untouched and return false.
    template <class K2>
    bool get_to(K2 const& k, V& out) const
        requires(std::is_copy_assignable_v<V>)
    {
        node* n = find_node(k, _hasher(k));
        if (n == nullptr)
            return false;
        out = n->value;
        return true;
    }

    /// The value for k if present, else a copy of fallback. Returns by value (fallback may be a temporary).
    template <class K2>
    [[nodiscard]] V get_or(K2 const& k, V const& fallback) const
        requires(std::is_copy_constructible_v<V>)
    {
        node* n = find_node(k, _hasher(k));
        return n != nullptr ? n->value : fallback;
    }

    /// The value for k if present, else a default-constructed V.
    template <class K2>
    [[nodiscard]] V get_or_default(K2 const& k) const
        requires(std::is_copy_constructible_v<V> && std::is_default_constructible_v<V>)
    {
        node* n = find_node(k, _hasher(k));
        return n != nullptr ? n->value : V();
    }

    // access / insert
public:
    /// Mutable subscript: returns the existing value, or inserts a default-constructed one (K from key).
    /// Enabled only when V is default-constructible and K is constructible from the key type.
    template <class K2>
    V& operator[](K2 const& k)
        requires(std::is_default_constructible_v<V> && std::is_constructible_v<K, K2 const&>)
    {
        u64 const h = _hasher(k);
        if (node* n = find_node(k, h))
            return n->value;
        return insert_node(h, k)->value;
    }

    /// const map has no subscript — subscripting default-inserts, which a const map cannot do.
    template <class K2>
    void operator[](K2 const&) const
    {
        static_assert(cc::impl::dependent_false<K2>, "const cc::map has no operator[]; use .get(key) or .entry(key)");
    }

    template <class K2>
    [[nodiscard]] entry_handle<std::decay_t<K2>> entry(K2 const& k)
    {
        using probe_t = std::decay_t<K2>;
        u64 const h = _hasher(k);
        return entry_handle<probe_t>(this, probe_t(k), h, find_node(k, h));
    }
    template <class K2>
    [[nodiscard]] const_entry_handle<K2> entry(K2 const& k) const
    {
        return const_entry_handle<K2>(find_node(k, _hasher(k)));
    }

    // mutation
public:
    /// Remove the entry for k if present; returns whether anything was removed.
    template <class K2>
    bool erase(K2 const& k)
    {
        auto const b = _buckets.obj_span();
        if (b.empty())
            return false;

        u64 const h = _hasher(k);
        node** link = &b[isize(h & (u64(b.size()) - 1))];
        while (node* n = *link)
        {
            if (n->hash == h && _eq(n->key, k))
            {
                *link = n->next;
                destroy_node(n);
                --_size;
                return true;
            }
            link = &n->next;
        }
        return false;
    }

    /// Destroy all entries but keep the bucket array (cleared to empty).
    void clear()
    {
        for (node*& head : _buckets.obj_span())
        {
            node* n = head;
            while (n != nullptr)
            {
                node* const next = n->next;
                destroy_node(n);
                n = next;
            }
            head = nullptr;
        }
        _size = 0;
    }

    /// Grow the bucket array so at least n entries fit without a rehash (load factor 1).
    void reserve(isize n)
    {
        isize const want = n < 8 ? 8 : isize(cc::bit_ceil(u64(n)));
        if (want > bucket_count())
            rehash_to(want);
    }

    // iteration
public:
    [[nodiscard]] iterator begin()
    {
        iterator it{this, isize(-1), nullptr};
        it.advance_to_valid();
        return it;
    }
    [[nodiscard]] iterator end() { return iterator{this, bucket_count(), nullptr}; }
    [[nodiscard]] const_iterator begin() const
    {
        const_iterator it{this, isize(-1), nullptr};
        it.advance_to_valid();
        return it;
    }
    [[nodiscard]] const_iterator end() const { return const_iterator{this, bucket_count(), nullptr}; }

    // internals
private:
    [[nodiscard]] cc::node_allocator& node_alloc() { return _node_res->get_allocator(_node_res->userdata); }

    static void destroy_node(node* n)
    {
        n->~node();
        cc::node_allocation_free(reinterpret_cast<cc::byte*>(n), cc::node_class_index_for<node>());
    }

    void destroy_all_nodes()
    {
        for (node* head : _buckets.obj_span())
        {
            while (head != nullptr)
            {
                node* const next = head->next;
                destroy_node(head);
                head = next;
            }
        }
    }

    template <class K2>
    [[nodiscard]] node* find_node(K2 const& k, u64 h) const
    {
        auto const b = _buckets.obj_span();
        if (b.empty())
            return nullptr;

        node* n = b[isize(h & (u64(b.size()) - 1))];
        while (n != nullptr)
        {
            if (n->hash == h && _eq(n->key, k))
                return n;
            n = n->next;
        }
        return nullptr;
    }

    // constructs a node (K from karg, V from vargs) and links it at the head of its bucket
    template <class KArg, class... VArgs>
    node* insert_node(u64 h, KArg&& karg, VArgs&&... vargs)
    {
        ensure_capacity_for_one();
        auto const b = _buckets.obj_span();
        node*& head = b[isize(h & (u64(b.size()) - 1))];

        auto* const raw = node_alloc().allocate_node_bytes(cc::node_class_index_for<node>(), sizeof(node), alignof(node));
        node* const n = new (cc::placement_new, raw) node(h, head, cc::forward<KArg>(karg), cc::forward<VArgs>(vargs)...);
        head = n;
        ++_size;
        return n;
    }

    void ensure_capacity_for_one()
    {
        isize const bc = bucket_count();
        if (bc == 0)
            rehash_to(initial_bucket_count);
        else if (_size + 1 > bc)
            rehash_to(bc * 2);
    }

    // move every node into a freshly allocated, zeroed bucket array of new_count (power of two)
    void rehash_to(isize new_count)
    {
        auto new_buckets = cc::allocation<node*>::create_defaulted(new_count, nullptr);
        auto const nb = new_buckets.obj_span();
        u64 const mask = u64(new_count) - 1;

        for (node* n : _buckets.obj_span())
        {
            while (n != nullptr)
            {
                node* const next = n->next;
                node*& head = nb[isize(n->hash & mask)];
                n->next = head;
                head = n;
                n = next;
            }
        }
        _buckets = cc::move(new_buckets); // frees the old bucket array (nodes already relinked)
    }

    void copy_from(map const& rhs)
    {
        isize const bc = rhs.bucket_count();
        if (bc == 0)
            return;

        _buckets = cc::allocation<node*>::create_defaulted(bc, nullptr);
        auto const nb = _buckets.obj_span();
        u64 const mask = u64(bc) - 1;

        for (node* head : rhs._buckets.obj_span())
        {
            for (node* n = head; n != nullptr; n = n->next)
            {
                auto* const raw
                    = node_alloc().allocate_node_bytes(cc::node_class_index_for<node>(), sizeof(node), alignof(node));
                node*& dst = nb[isize(n->hash & mask)];
                dst = new (cc::placement_new, raw) node(n->hash, dst, n->key, n->value);
                ++_size;
            }
        }
    }

    // members
private:
    static constexpr isize initial_bucket_count = 8;

    cc::allocation<node*> _buckets; // power-of-two length; empty until first insert
    isize _size = 0;                // number of live entries
    cc::node_memory_resource* _node_res = cc::default_node_memory_resource; // backs the K/V nodes
    [[no_unique_address]] Hash _hasher{};
    [[no_unique_address]] KeyEqual _eq{};
};
