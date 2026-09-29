#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::impl
{
class aliasing_scope;
}

/// What one dispatch or draw binds, held to WebGPU's rule where `context::portability_checks` is on.
/// **A buffer one binding writes is read through no other binding of the dispatch or draw**, whatever the ranges.
/// Two bindings that both write one buffer are allowed, as WebGPU allows them, and a binding array is not seen at all.
/// Two draws are never checked against each other: a backend splits the pass between them where one needs the other's
/// write, which is where WebGPU's usage scope ends too.
class sg::impl::aliasing_scope
{
public:
    /// Forgets everything, for the next dispatch or draw.
    void clear() { _uses.clear(); }

    /// Adds what the dispatch or draw binds, and asserts, naming both bindings, where it breaks the rule.
    /// `groups` is by group slot and `vertex_buffers` by vertex slot, with null for an empty one.
    /// `index_buffer` is null for a draw that reads none.
    void add(cc::span<binding_group const* const> groups,
             cc::span<raw_buffer const* const> vertex_buffers,
             raw_buffer const* index_buffer);

private:
    struct use
    {
        raw_buffer const* buffer = nullptr;
        bool writes = false;
        int group = -1;          // the group slot, or -1 for a vertex or index buffer
        cc::string_view binding; // within the group, which the recording keeps alive
        int vertex_slot = -1;    // or -1 for the index buffer
    };

    void add(use const& u);

    cc::vector<use> _uses;
};
