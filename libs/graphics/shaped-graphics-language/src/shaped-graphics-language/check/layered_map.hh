#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/container/map.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <shaped-graphics-language/fwd.hh>

/// One of the check pass's scopes or caches, split into what the layers below filled and what this layer adds.
///
/// The `layered` of a map: the lower part is shared and never written, so a program's check behind the prelude reads
/// the prelude's scopes in place rather than copying them.
/// A key this layer writes is its own from then on: writing one the lower part holds copies that entry up first, so an
/// extension of a prelude type, or another overload of a prelude operator, lands in this layer alone.
/// Iterating yields every key once, this layer's value where it has one.
template <class K, class V>
class sgl::check::layered_map
{
    using map = cc::map<K, V>;

public:
    template <class K2>
    [[nodiscard]] V const* get_ptr(K2 const& key) const
    {
        if (auto const* const own = _own.get_ptr(key))
            return own;
        return _base == nullptr ? nullptr : _base->get_ptr(key);
    }

    template <class K2>
    [[nodiscard]] bool contains(K2 const& key) const
    {
        return get_ptr(key) != nullptr;
    }

    /// The entry of `key` to write, which is this layer's own from here on.
    template <class K2>
    V& operator[](K2 const& key)
    {
        if (auto* const own = _own.get_ptr(key))
            return *own;
        if (_base != nullptr)
            if (auto const* const lower = _base->get_ptr(key))
                return _own[key] = *lower;
        return _own[key];
    }

    /// The entries this layer wrote, each of which it took over from the lower part or added.
    [[nodiscard]] map const& own_entries() const { return _own; }

    /// Whether both are the one frozen map, neither with entries of its own: what a layer that added nothing shares.
    [[nodiscard]] bool shares_all_with(layered_map const& other) const
    {
        return _own.size() == 0 && other._own.size() == 0 && _base == other._base;
    }

    /// Makes every entry part of the shared lower layer, which the layer above starts from and never writes.
    void freeze()
    {
        if (_own.size() == 0)
            return;
        auto all = map();
        if (_base != nullptr)
            for (auto const& [key, value] : *_base)
                if (!_own.contains(key))
                    all[key] = value;
        for (auto const& [key, value] : _own)
            all[key] = value;
        _own = {};
        _base = cc::make_shared<map const>(cc::move(all));
    }

    struct iterator
    {
        using inner = typename map::const_iterator;

        layered_map const* table = nullptr;
        inner at;
        bool in_base = false;

        [[nodiscard]] decltype(auto) operator*() const { return *at; }
        iterator& operator++()
        {
            ++at;
            skip();
            return *this;
        }
        [[nodiscard]] bool operator==(iterator const& rhs) const { return in_base == rhs.in_base && at == rhs.at; }
        [[nodiscard]] bool operator!=(iterator const& rhs) const { return !(*this == rhs); }

        /// Past a lower entry this layer overrides, and from the end of the lower part on to this layer's own.
        void skip()
        {
            while (in_base)
            {
                if (at == table->_base->end())
                {
                    in_base = false;
                    at = table->_own.begin();
                    return;
                }
                auto const& [key, value] = *at;
                (void)value;
                if (!table->_own.contains(key))
                    return;
                ++at;
            }
        }
    };
    [[nodiscard]] iterator begin() const
    {
        if (_base == nullptr)
            return {.table = this, .at = _own.begin(), .in_base = false};
        auto it = iterator{.table = this, .at = _base->begin(), .in_base = true};
        it.skip();
        return it;
    }
    [[nodiscard]] iterator end() const { return {.table = this, .at = _own.end(), .in_base = false}; }

private:
    cc::shared_ptr<map const> _base;
    map _own;
};
