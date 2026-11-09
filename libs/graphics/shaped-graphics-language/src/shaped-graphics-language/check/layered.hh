#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/fwd.hh>

/// One table of the check pass, split into what the layers below filled and what this layer adds.
///
/// The lower part is shared, read by any number of checks on any number of threads, and never written: so a program's
/// check behind the prelude reads the prelude's entries in place rather than copying them (the compilation model's
/// checked layers, `libs/graphics/shaped-graphics-language/docs/spec/incubator/compilation-model.md`).
/// Ids stay what they were: entry `i` is the `i`th of the whole table, whichever part holds it.
/// A range of entries is always allocated in one go, so it never spans the two parts.
template <class T>
class sgl::check::layered
{
public:
    [[nodiscard]] isize size() const { return _base_size + _own.size(); }
    [[nodiscard]] bool empty() const { return size() == 0; }

    /// Reading is all indexing does: a write names `own`, so every write is one a reader can find.
    [[nodiscard]] T const& operator[](isize i) const { return i < _base_size ? (*_base)[i] : _own[i - _base_size]; }
    /// Entry `i` to write, which must be this layer's own: a lower layer's is shared.
    [[nodiscard]] T& own(isize i)
    {
        CC_ASSERT(i >= _base_size, "a lower layer's entry is shared, and never written");
        return _own[i - _base_size];
    }

    [[nodiscard]] T const& back() const { return (*this)[size() - 1]; }
    [[nodiscard]] T& own_back() { return own(size() - 1); }

    void push_back(T value) { _own.push_back(cc::move(value)); }
    void push_back_range(cc::span<T const> values) { _own.push_back_range(values); }
    void remove_back()
    {
        CC_ASSERT(!_own.empty(), "only this layer's own entries are removed");
        _own.remove_back();
    }

    /// The `count` entries from `first`, which lie in one part.
    [[nodiscard]] cc::span<T const> slice(isize first, isize count) const
    {
        if (first >= _base_size)
            return cc::span<T const>(_own).subspan({.offset = first - _base_size, .size = count});
        CC_ASSERT(first + count <= _base_size, "a range of entries lies in one layer");
        return cc::span<T const>(*_base).subspan({.offset = first, .size = count});
    }

    /// Makes every entry part of the shared lower layer, which the layer above starts from and never writes.
    void freeze()
    {
        if (_own.empty())
            return;
        auto all = cc::vector<T>();
        all.reserve(size());
        for (auto i = isize(0); i < _base_size; ++i)
            all.push_back((*_base)[i]);
        for (auto& e : _own)
            all.push_back(cc::move(e));
        _own = {};
        _base_size = all.size();
        _base = cc::make_shared<cc::vector<T> const>(cc::move(all));
    }

    /// How many entries the shared lower layer holds.
    [[nodiscard]] isize shared_size() const { return _base_size; }

    /// Whether both are the one frozen table, neither with entries of its own: what a layer that added nothing shares.
    [[nodiscard]] bool shares_all_with(layered const& other) const
    {
        return _own.empty() && other._own.empty() && _base == other._base && _base_size == other._base_size;
    }

    struct iterator
    {
        layered const* table = nullptr;
        isize index = 0;

        [[nodiscard]] T const& operator*() const { return (*table)[index]; }
        [[nodiscard]] T const* operator->() const { return &(*table)[index]; }
        iterator& operator++()
        {
            ++index;
            return *this;
        }
        [[nodiscard]] bool operator==(iterator const& rhs) const { return index == rhs.index; }
        [[nodiscard]] bool operator!=(iterator const& rhs) const { return index != rhs.index; }
    };
    [[nodiscard]] iterator begin() const { return {.table = this, .index = 0}; }
    [[nodiscard]] iterator end() const { return {.table = this, .index = size()}; }

private:
    cc::shared_ptr<cc::vector<T> const> _base;
    isize _base_size = 0;
    cc::vector<T> _own;
};

namespace sgl::ast::impl
{
template <class T>
[[nodiscard]] cc::span<T const> slice(check::layered<T> const& all, range_of<T> r)
{
    return all.slice(r.first, r.count);
}

template <class T>
[[nodiscard]] bool is_equal(check::layered<T> const& a, check::layered<T> const& b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}
} // namespace sgl::ast::impl
