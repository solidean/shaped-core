#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <memory>

namespace sg::backend::dx12
{
/// Logical query-heap categories the query system pools. Only timestamps exist today; occlusion /
/// pipeline-statistics would slot in here and reuse the same lease/resolve/download machinery.
enum class dx12_query_heap_type : cc::u32
{
    timestamp = 0,

    count
};

/// One ID3D12QueryHeap leased exclusively by a single command list while recording, returned to the
/// pool after submit/drop. Slots are bump-allocated (next_slot). Every gpu_timestamp pointing into this
/// heap shares `shared_future`, which is assigned in place at submit to the heap's readback.
struct dx12_query_heap_lease
{
    ComPtr<ID3D12QueryHeap> heap;
    dx12_query_heap_type type = dx12_query_heap_type::timestamp;
    int slot_count = 0;
    int next_slot = 0;

    /// Shared by every handle pointing into this heap. Default-constructed (invalid) until submit, then
    /// assigned in place with the heap's actual readback; a dropped list leaves it invalid forever.
    std::shared_ptr<sg::data_future<cc::u64>> shared_future = std::make_shared<sg::data_future<cc::u64>>();
};

/// Backend GPU-query system: a free-list pool of small (SlotsPerHeap-slot) ID3D12QueryHeaps. A command
/// list leases a heap on demand, bump-allocates slots as queries are recorded, and at submit resolves
/// the heap into a transient buffer + starts one inline readback (see
/// dx12_command_list::finalize_queries_before_close). Heaps return to the pool right after.
///
/// All public methods are threadsafe; multiple command lists may lease and release heaps concurrently.
class dx12_query_system
{
public:
    /// Slots per query heap. Intentionally small (mirrors tighter limits of other APIs, bounds each
    /// readback chunk). A list recording more timestamps than this simply leases additional heaps.
    static constexpr int SlotsPerHeap = 4096;

    explicit dx12_query_system(dx12_context& ctx) : _ctx(ctx) {}

    /// Caches the direct queue's timestamp frequency (→ tick→seconds factor) and marks timestamp support.
    /// Called once during context bring-up, after the queue exists. Returns a dx12 error if the frequency
    /// query fails.
    [[nodiscard]] cc::result<cc::unit> initialize();

    /// Whether GPU timestamps are supported (frequency query succeeded at initialize).
    [[nodiscard]] bool supports_timestamps() const { return _supports_timestamps; }

    /// Multiplier from raw direct-queue ticks to seconds (1 / GetTimestampFrequency).
    [[nodiscard]] double timestamp_tick_to_seconds() const { return _timestamp_tick_to_seconds; }

    /// Leases a heap of `type` with all slots free. Pulls from the free list or creates a new heap.
    [[nodiscard]] cc::unique_ptr<dx12_query_heap_lease> acquire_heap(dx12_query_heap_type type);

    /// Returns a heap to the pool: resets next_slot and installs a fresh (invalid) shared_future for the
    /// next leaseholder. Handles from the previous lease keep their own (now-real) future.
    void release_heap(cc::unique_ptr<dx12_query_heap_lease> lease);

    /// Drops all pooled heaps. Called from context shutdown.
    void shutdown();

private:
    [[nodiscard]] ComPtr<ID3D12QueryHeap> create_heap(dx12_query_heap_type type);

    dx12_context& _ctx;
    bool _supports_timestamps = false;
    double _timestamp_tick_to_seconds = 0.0;

    // Free list per query type. Heaps here have next_slot == 0 and a fresh invalid shared_future.
    cc::mutex<cc::vector<cc::unique_ptr<dx12_query_heap_lease>>> _free_list_by_type[int(dx12_query_heap_type::count)];
};
} // namespace sg::backend::dx12
