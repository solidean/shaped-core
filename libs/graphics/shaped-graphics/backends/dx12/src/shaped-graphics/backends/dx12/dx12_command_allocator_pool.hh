#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>

#include <array>

namespace sg::backend::dx12
{
/// A command allocator tagged with the queue type it was created for. The type is fixed at creation
/// (D3D12 requires the allocator's type to match the lists recorded onto it), so it must be routed
/// back to that queue's free pool. Allocators can only be reset once every list sourced from them
/// has finished on the GPU — hence they are epoch-gated.
struct dx12_pooled_allocator
{
    ComPtr<ID3D12CommandAllocator> allocator;
    D3D12_COMMAND_LIST_TYPE queue = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

/// A freshly acquired allocator paired with an open (recording) command list.
struct dx12_acquired_command_list
{
    dx12_pooled_allocator allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};

/// Pools command allocators and command lists for reuse, partitioned per queue type.
///
/// Allocators and lists have different lifetimes: an allocator's memory backs GPU execution, so it is
/// epoch-gated — held in-flight until the epoch that submitted it retires, then reset and reused. A
/// command list can be reset (onto a fresh, GPU-safe allocator) while its own previous submission is
/// still executing, so lists return to the pool immediately and are never epoch-gated. Reusing a list
/// avoids the per-list CreateCommandList cost, which dominates command-list creation.
///
/// Thread-safe: create / submit / drop run concurrently. Each queue has its own mutex so recording on
/// different queues never contends.
class dx12_command_allocator_pool
{
public:
    explicit dx12_command_allocator_pool(dx12_context& ctx) : _ctx(ctx) {}

    /// Acquires an allocator plus an open, recording command list for `queue`. Both are reused from
    /// the pool when available (allocator reset, list reset onto it), else freshly created.
    [[nodiscard]] cc::result<dx12_acquired_command_list> acquire_command_list(D3D12_COMMAND_LIST_TYPE queue);

    /// Returns a submitted list's allocator: held in-flight until the current epoch retires, then
    /// reset and reused. Not reset here — it may still back GPU work.
    void return_submitted_allocator(dx12_pooled_allocator allocator);

    /// Returns a never-submitted allocator straight to the free pool (the GPU never touched it; the
    /// reset happens at the next acquire).
    void return_free_allocator(dx12_pooled_allocator allocator);

    /// Returns a CLOSED command list for reuse. The list is reset onto its next allocator at acquire,
    /// so returning it while its previous submission is still in flight is legal.
    void return_command_list(D3D12_COMMAND_LIST_TYPE queue, ComPtr<ID3D12GraphicsCommandList> list);

    /// Drains every allocator captured by this epoch's submissions across all queues, tagged with its
    /// queue. Called at epoch advance; the caller stores the batch in the epoch payload and hands it
    /// back to reclaim_allocators once the epoch's GPU work is done.
    [[nodiscard]] cc::vector<dx12_pooled_allocator> drain_in_epoch_allocators();

    /// Resets finished allocators and returns them to their queue's free pool. Every list sourced from
    /// them must have finished on the GPU (guaranteed by the epoch that owned them having retired).
    void reclaim_allocators(cc::vector<dx12_pooled_allocator> allocators);

    /// Drops all pooled allocators and lists. Call once the GPU is idle and no more work will arrive.
    void shutdown();

    // Introspection for tests and future stats.
    [[nodiscard]] cc::isize free_allocator_count(D3D12_COMMAND_LIST_TYPE queue);
    [[nodiscard]] cc::isize free_command_list_count(D3D12_COMMAND_LIST_TYPE queue);

private:
    dx12_context& _ctx; // creating context — outlives this pool

    // Indexes D3D12_COMMAND_LIST_TYPE directly: DIRECT=0, BUNDLE=1, COMPUTE=2, COPY=3, VIDEO_DECODE=4,
    // VIDEO_PROCESS=5, VIDEO_ENCODE=6.
    static constexpr int queue_count = 7;
    static_assert(D3D12_COMMAND_LIST_TYPE_DIRECT < queue_count, "not enough queue slots");
    static_assert(D3D12_COMMAND_LIST_TYPE_COMPUTE < queue_count, "not enough queue slots");
    static_assert(D3D12_COMMAND_LIST_TYPE_COPY < queue_count, "not enough queue slots");
    static_assert(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE < queue_count, "not enough queue slots");

    struct per_queue_pool
    {
        cc::vector<ComPtr<ID3D12CommandAllocator>> free_allocators; // idle, reset, ready for reuse
        cc::vector<ComPtr<ID3D12CommandAllocator>> in_epoch;        // captured by submitted lists this epoch
        cc::vector<ComPtr<ID3D12GraphicsCommandList>> free_lists;   // closed, ready to reset + reuse
    };

    std::array<cc::mutex<per_queue_pool>, queue_count> _by_queue;
};
} // namespace sg::backend::dx12
