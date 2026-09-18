#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>

/// One uniform page: a buffer, its host mirror, and how much of it the leasing list has filled.
struct sg::backend::webgpu::webgpu_constant_page
{
    /// The page's position in the context's page list, which is what a pipeline layout caches its group 3 by.
    isize index = 0;

    wgpu_buffer buffer;
    cc::vector<byte> mirror;
    isize head = 0;

    /// The last block written, so an unchanged block is bound again at the same offset rather than written twice.
    isize last_offset = -1;
    isize last_size = 0;
};

/// The pages behind inline constants, which WebGPU does not have.
///
/// A list's inline constants are written into a page leased to that list, at an offset aligned to the adapter's minimum uniform offset alignment, and bound as group 3 binding 0 with a dynamic offset.
/// The page is written with one `queue.writeBuffer` just before the list submits, and returns to the free list in that same step, still ahead of the submit.
/// That is safe because the next write to it can only come from a later submit's flush, and queue order puts that write after this list's submit.
///
/// An unchanged block binds again at the offset it already has, so a list that sets the same constants for every draw costs one block.
class sg::backend::webgpu::webgpu_constant_pages
{
public:
    void initialize(webgpu_context& ctx, isize page_bytes, isize offset_alignment);
    void shutdown();

    /// Where `block` lands for a list leasing `leased`, leasing another page when the last one is full.
    struct placement
    {
        webgpu_constant_page* page = nullptr;
        u32 offset = 0;
    };
    [[nodiscard]] placement place(cc::vector<webgpu_constant_page*>& leased, cc::span<byte const> block);

    /// Writes every leased page and returns them; the caller submits right after.
    void flush_and_return(cc::vector<webgpu_constant_page*>& leased);

    /// Returns every leased page unwritten; the list was dropped.
    void discard(cc::vector<webgpu_constant_page*>& leased);

    [[nodiscard]] isize page_bytes() const { return _page_bytes; }

private:
    [[nodiscard]] webgpu_constant_page* lease();
    void give_back(webgpu_constant_page* page);

    webgpu_context* _ctx = nullptr;
    isize _page_bytes = 0;
    isize _alignment = 256;
    cc::vector<std::unique_ptr<webgpu_constant_page>> _pages;
    cc::vector<webgpu_constant_page*> _free;
};
