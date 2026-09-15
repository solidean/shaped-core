#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>

/// One transfer window as it was submitted.
struct sg::backend::vulkan::vulkan_transfer_window_record
{
    u64 window_value = 0; // on the system's window timeline
    int slot = 0;         // which of the three command buffers carried it
    u64 sequence = 0;     // the job's submission order, which the scheduler keeps per destination
    u64 destination = 0;  // the VkBuffer or VkImage handle, as validation messages print it
    bool is_texture = false;
    i64 offset = 0; // into the destination (upload) or the source (download)
    i64 bytes = 0;
    u64 wait_token = 0;       // the direct-queue submission this window waited on, or 0
    u64 cross_wait_value = 0; // the other direction's timeline value it waited on, or 0
    u64 completion_value = 0; // the completion value this window signalled, or 0
};

/// The last windows a transfer system submitted, readable from any thread.
///
/// Kept for one question: when synchronization validation reports a hazard between two copies, which copies were they,
/// did their ranges overlap, and were they queued in the order they were submitted.
/// A validation message names the command buffer and the resource, and none of the rest.
class sg::backend::vulkan::vulkan_transfer_window_log
{
public:
    void note(vulkan_transfer_window_record const& record)
    {
        _state.lock(
            [&](state& s)
            {
                s.records[s.next] = record;
                s.next = (s.next + 1) % k_capacity;
                s.count = s.count < k_capacity ? s.count + 1 : k_capacity;
            });
    }

    /// Oldest first, one line per window.
    [[nodiscard]] cc::string describe(cc::string_view system_name)
    {
        return _state.lock(
            [&](state& s)
            {
                auto out = cc::format("last {} {} windows, oldest first:\n", s.count, system_name);
                for (isize i = 0; i < s.count; ++i)
                {
                    auto const& r = s.records[(s.next - s.count + i + k_capacity) % k_capacity];
                    out += cc::format("  window {} slot {} seq {} {} 0x{:x} offset {} bytes {} wait_token {} "
                                      "cross_wait {} completion {}\n",
                                      r.window_value, r.slot, r.sequence, r.is_texture ? "image" : "buffer",
                                      r.destination, r.offset, r.bytes, r.wait_token, r.cross_wait_value,
                                      r.completion_value);
                }
                return out;
            });
    }

private:
    static constexpr isize k_capacity = 32;

    struct state
    {
        vulkan_transfer_window_record records[k_capacity];
        isize next = 0;
        isize count = 0;
    };
    cc::mutex<state> _state;
};
