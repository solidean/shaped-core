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
/// A range of entries is always allocated in one go, so it never spans two segments, nor the lower part and the own.
///
/// The lower part is a few shared segments rather than one vector, so a chain of layers shares what is below each.
/// A freeze merges the newest segments while the last is no more than twice the size of the next, so there are about
/// log2 of the table's size of them, and a freeze copies the newest entries rather than the whole table.
template <class T>
class sgl::check::layered
{
    struct segment
    {
        isize start = 0;
        isize end = 0;
        T const* data = nullptr;
        cc::shared_ptr<cc::vector<T> const> entries;
    };
    /// Entries per step of `frozen::directory`.
    static constexpr isize k_step_bits = 8;
    struct frozen
    {
        cc::vector<segment> segments;
        /// For each run of `1 << k_step_bits` entries, the segment its first entry lies in.
        cc::vector<u32> directory;
    };

    [[nodiscard]] segment const& segment_of(isize i) const
    {
        auto const& f = *_frozen;
        auto s = isize(f.directory[i >> k_step_bits]);
        while (i >= f.segments[s].end)
            ++s;
        return f.segments[s];
    }

public:
    [[nodiscard]] isize size() const { return _base_size + _own.size(); }
    /// The first entry this layer owns; every one before it is a lower layer's.
    [[nodiscard]] isize first_own() const { return _base_size; }
    [[nodiscard]] bool empty() const { return size() == 0; }

    /// Reading is all indexing does: a write names `own`, so every write is one a reader can find.
    [[nodiscard]] T const& operator[](isize i) const
    {
        // the first segment, the prelude's, is where most lookups land
        if (i < _first_end)
            return _first[i];
        if (i >= _base_size)
            return _own[i - _base_size];
        auto const& s = segment_of(i);
        return s.data[i - s.start];
    }
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
        auto const& s = segment_of(first);
        CC_ASSERT(first + count <= s.end, "a range of entries lies in one segment");
        return cc::span<T const>(s.data + (first - s.start), count);
    }

    /// Makes every entry part of the shared lower layer, which the layer above starts from and never writes.
    void freeze()
    {
        if (_own.empty())
            return;
        auto segments = _frozen != nullptr ? _frozen->segments : cc::vector<segment>();
        // the newest segments merged with the own entries while the last is small beside them
        auto merged = cc::move(_own);
        _own = {};
        auto start = _base_size;
        while (!segments.empty() && segments.back().end - segments.back().start <= 2 * merged.size())
        {
            auto const& last = segments.back();
            auto joined = cc::vector<T>();
            joined.reserve((last.end - last.start) + merged.size());
            joined.push_back_range(cc::span<T const>(last.data, last.end - last.start));
            for (auto& e : merged)
                joined.push_back(cc::move(e));
            start = last.start;
            merged = cc::move(joined);
            segments.remove_back();
        }
        auto entries = cc::make_shared<cc::vector<T> const>(cc::move(merged));
        segments.push_back(
            {.start = start, .end = start + entries->size(), .data = entries->data(), .entries = cc::move(entries)});
        _base_size = segments.back().end;
        auto f = frozen{.segments = cc::move(segments)};
        auto s = u32(0);
        for (auto step = isize(0); step < _base_size; step += isize(1) << k_step_bits)
        {
            while (step >= f.segments[s].end)
                ++s;
            f.directory.push_back(s);
        }
        _first = f.segments.front().data;
        _first_end = f.segments.front().end;
        _frozen = cc::make_shared<frozen const>(cc::move(f));
    }

    /// How many entries the shared lower layer holds.
    [[nodiscard]] isize shared_size() const { return _base_size; }

    /// Whether both are the one frozen table, neither with entries of its own: what a layer that added nothing shares.
    [[nodiscard]] bool shares_all_with(layered const& other) const
    {
        return _own.empty() && other._own.empty() && _frozen == other._frozen && _base_size == other._base_size;
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
    cc::shared_ptr<frozen const> _frozen;
    /// The first segment's entries, held here so a lookup there takes no indirection through `_frozen`.
    T const* _first = nullptr;
    isize _first_end = 0;
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
