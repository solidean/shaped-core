#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/allocation.hh>
#include <versioned-document/fwd.hh>

/// Internal to vdoc: where a typed document's storage comes from.
/// Both the parser and document_builder allocate through it, which is why it is a header rather than a detail of
/// document.cc.

/// A bump allocator behind a cc::memory_resource, so a whole document's storage frees in one release.
///
/// **TEMPORARY, and vdoc-local.**
/// This belongs in clean-core as a general bump resource — a `cc::arena_memory_resource` next to
/// `cc::system_memory_resource` — and this copy goes when that lands; `cc::impl::intern_shard` already hand-rolls the
/// same thing privately, which is the second copy that makes it worth having once.
/// See [decisions.md](../../../docs/decisions.md#the-document-arena-is-vdoc-local-until-clean-core-grows-one).
///
/// Deallocation is a no-op and there is no in-place resize, so a container backed by this must be created at its final
/// capacity and never grown.
/// A builder that outgrows one allocates a fresh array and reports the old one as dead.
class vdoc::impl::document_arena
{
    // lifetime
public:
    document_arena()
    {
        _resource.allocate_bytes = &allocate_through;
        _resource.deallocate_bytes = &deallocate_through;
        _resource.userdata = this;
    }

    ~document_arena()
    {
        for (auto const& b : _blocks)
            cc::default_memory_resource->deallocate_bytes(b.data, b.size, block_alignment,
                                                          cc::default_memory_resource->userdata);
    }

    document_arena(document_arena const&) = delete;
    document_arena(document_arena&&) = delete;
    document_arena& operator=(document_arena const&) = delete;
    document_arena& operator=(document_arena&&) = delete;

    // allocation
public:
    /// Uninitialized bytes valid until the arena dies.
    /// `alignment` must be a power of two, and `bytes` may be 0.
    [[nodiscard]] byte* allocate(isize bytes, isize alignment)
    {
        CC_ASSERT(bytes >= 0 && alignment > 0, "a bump allocation needs a non-negative size and a positive alignment");
        if (bytes == 0)
            return nullptr;

        _allocated_bytes += bytes;

        if (!_blocks.empty())
        {
            auto& b = _blocks.back();
            auto const aligned = align_up(b.used, alignment);
            if (aligned + bytes <= b.size)
            {
                b.used = aligned + bytes;
                return b.data + aligned;
            }
        }

        // An oversized request gets its own block rather than burning the doubling sequence on it.
        auto const next = _next_block_size < bytes + alignment ? bytes + alignment : _next_block_size;
        add_block(next);
        _next_block_size = next * 2;

        auto& b = _blocks.back();
        auto const aligned = align_up(b.used, alignment);
        b.used = aligned + bytes;
        return b.data + aligned;
    }

    /// Records bytes nothing points at any more, which only an in-place edit produces.
    ///
    /// A bump arena cannot reclaim them, so a builder that grows a column repeatedly leaves the old arrays behind.
    /// Counting is what lets a caller decide when compacting is worth relocating everything.
    void note_dead(isize bytes) { _dead_bytes += bytes; }

    [[nodiscard]] isize dead_bytes() const { return _dead_bytes; }

    /// Bytes handed out that something still points at.
    [[nodiscard]] isize live_bytes() const { return _allocated_bytes - _dead_bytes; }

    [[nodiscard]] cc::memory_resource const* as_memory_resource() const { return &_resource; }

private:
    struct block
    {
        byte* data = nullptr;
        isize size = 0;
        isize used = 0;
    };

    static constexpr isize block_alignment = 16;

    [[nodiscard]] static isize align_up(isize v, isize alignment) { return (v + alignment - 1) & ~(alignment - 1); }

    void add_block(isize size)
    {
        byte* data = nullptr;
        auto const actual = cc::default_memory_resource->allocate_bytes(&data, size, size, block_alignment,
                                                                        cc::default_memory_resource->userdata);
        _blocks.push_back({.data = data, .size = actual, .used = 0});
    }

    static isize allocate_through(byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
    {
        if (min_bytes == 0)
        {
            *out_ptr = nullptr;
            return 0;
        }

        // The arena hands out exactly what was asked for; there is no size class to round up into.
        (void)max_bytes;
        *out_ptr = static_cast<document_arena*>(userdata)->allocate(min_bytes, alignment);
        return min_bytes;
    }

    static void deallocate_through(byte*, isize, isize, void*) {}

    cc::vector<block> _blocks;
    isize _next_block_size = 16 * 1024;
    isize _allocated_bytes = 0;
    isize _dead_bytes = 0;
    cc::memory_resource _resource = {};
};
