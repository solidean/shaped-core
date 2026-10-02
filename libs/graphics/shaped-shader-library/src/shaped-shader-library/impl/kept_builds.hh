#pragma once

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh> // slib::shader_option

namespace slib::impl
{
/// What a pipeline was last described with, per context and per set of option values, while its frozen part still
/// matched the build: the stages the host's code fits, which a reload that moves the frozen part falls back to.
///
/// A set of values is matched as a shader asset matches its key, by name in any order, so the values of one set never
/// stand in for another's.
/// Not synchronized: its owner holds a lock around every call.
template <class Built>
struct kept_builds
{
    struct entry
    {
        sg::context const* ctx = nullptr;
        /// Ordered by name.
        cc::vector<shader_option> options;
        Built built;
    };
    cc::vector<entry> entries;

    /// While nothing moved, keeps `built` for (`ctx`, `options`); once something did, replaces `built` with what was
    /// kept for that pair.
    /// False where something moved before anything was kept for that pair, which leaves `built` as it was.
    [[nodiscard]] bool keep_or_restore(sg::context const* ctx,
                                       cc::span<shader_option const> options,
                                       bool is_moved,
                                       Built& built)
    {
        auto key = cc::vector<shader_option>::create_copy_of(options);
        cc::sort(key, [](shader_option const& a, shader_option const& b) { return a.name < b.name; });
        for (auto& e : entries)
            if (e.ctx == ctx && is_same(e.options, key))
            {
                if (is_moved)
                    built = e.built;
                else
                    e.built = built;
                return true;
            }
        if (is_moved)
            return false;
        entries.push_back({.ctx = ctx, .options = cc::move(key), .built = built});
        return true;
    }

private:
    static bool is_same(cc::span<shader_option const> a, cc::span<shader_option const> b)
    {
        if (a.size() != b.size())
            return false;
        for (auto i = isize(0); i < a.size(); ++i)
            if (!(a[i] == b[i]))
                return false;
        return true;
    }
};
} // namespace slib::impl
