#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/bytes_future.hh>

/// One timestamp query set leased by a command list, and the future its ticks download into.
struct sg::backend::webgpu::webgpu_query_lease
{
    wgpu_query_set query_set;
    wgpu_buffer resolve; // QUERY_RESOLVE | COPY_SRC, one u64 per query
    int used = 0;

    /// Handed to every timestamp recorded into this lease, and assigned in place at submit.
    std::shared_ptr<sg::data_future<u64>> future;
};

/// The query sets behind `cmd.query`, present only where the device has `timestamp-query`.
///
/// WebGPU writes timestamps only at the start or end of a pass, never at an arbitrary point.
/// So a recorded timestamp ends whatever pass is open and runs an empty compute pass whose beginning writes the query, which is as close to "now" as the API can say.
/// Ticks are nanoseconds.
class sg::backend::webgpu::webgpu_query_system
{
public:
    static constexpr int queries_per_set = 32;

    /// The last slot of every set is a discard target; see query_record_gpu_timestamp.
    static constexpr int usable_queries_per_set = queries_per_set - 1;

    void initialize(webgpu_context& ctx, bool supported);
    void shutdown();

    [[nodiscard]] bool is_supported() const { return _supported; }

    /// A lease with room for another query, reusing an idle one.
    [[nodiscard]] webgpu_query_lease* lease();

    /// Returns a lease whose readback has been recorded or discarded.
    void give_back(webgpu_query_lease* lease);

private:
    webgpu_context* _ctx = nullptr;
    bool _supported = false;
    cc::vector<std::unique_ptr<webgpu_query_lease>> _leases;
    cc::vector<webgpu_query_lease*> _free;
};
