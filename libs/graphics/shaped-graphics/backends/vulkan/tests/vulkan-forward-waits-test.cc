#include "vulkan-test-common.hh"

#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <nexus/async-test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/vulkan/vulkan_forward_waits.hh>
#include <shaped-graphics/transfer/stream_source.hh>

#include <atomic>
#include <thread>

using namespace cc::primitive_defines;

// device_driver_barrier keeps vkCreateDevice clear of queued wait-before-signal — see vulkan_forward_waits.hh.
// A stream fed by caller code is the one signal the barrier cannot hurry, so it waits for the source.

namespace
{
/// Opened from the test's thread, and outliving the source: once opened, the actor may finish and destroy the source
/// before release() has returned.
struct gate
{
    std::atomic<bool> open = false;
    cc::mutex<cc::unique_function<void()>> waker; // installed on the actor thread

    void release()
    {
        open.store(true, std::memory_order_release);
        waker.lock(
            [](cc::unique_function<void()>& w)
            {
                if (w)
                    w();
            });
    }
};

/// Withholds its bytes until its gate opens, as a loader whose data is not back yet.
class gated_source final : public sg::stream_source
{
public:
    gated_source(cc::span<byte const> bytes, std::shared_ptr<gate> g) : _bytes(bytes), _gate(cc::move(g)) {}

    [[nodiscard]] sg::stream_poll try_next_chunk() override
    {
        if (!_gate->open.load(std::memory_order_acquire))
            return {.status = sg::stream_source_status::not_yet};
        if (_handed_over)
            return {.status = sg::stream_source_status::done};
        _handed_over = true;
        return {.status = sg::stream_source_status::ready,
                .chunk = {.data = cc::pinned_data<byte>::create_copy_of(_bytes)}};
    }

    void set_waker(cc::unique_function<void()> waker) override
    {
        _gate->waker.lock([&](cc::unique_function<void()>& w) { w = cc::move(waker); });
    }

private:
    cc::span<byte const> _bytes;
    std::shared_ptr<gate> _gate;
    bool _handed_over = false;
};
} // namespace

ASYNC_TEST("sg vulkan - a device create waits for a stream a submitted list is waiting on", exclusive("vulkan-device"))
{
#if CC_HAS_THREADS
    auto const ctx = sg::backend::vulkan::test::make_context();
    if (ctx == nullptr)
        SKIP("no Vulkan device on this host");
    REQUIRE(ctx != nullptr);

    auto second = sg::backend::vulkan::vulkan_context_handle();
    {
        auto bytes = cc::vector<byte>::create_filled(256, byte(7));
        auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
        auto const opener = std::make_shared<gate>();
        auto source = std::make_unique<gated_source>(cc::span<byte const>(bytes), opener);
        auto stream = ctx->stream.from_source_to_buffer(buf, cc::move(source));
        stream.promote_to_async(); // the list below waits on the stream on purpose

        // A queued GPU wait on the stream's completion value, which only the source's `done` will ever signal.
        auto cmd = ctx->create_command_list();
        auto const readback = cmd->download.bytes_from_buffer(buf, 0, 256);
        ctx->submit_command_list(cc::move(cmd));
        REQUIRE(sg::backend::vulkan::pending_forward_wait_timelines() > 0);

        auto created = std::atomic<bool>(false);
        auto finished = std::atomic<bool>(false);
        auto creator = std::thread(
            [&]
            {
                second = sg::backend::vulkan::test::make_context();
                created.store(second != nullptr, std::memory_order_release);
                finished.store(true, std::memory_order_release);
            });

        // Waits for the creating thread to reach its barrier, which it cannot leave while the source withholds its bytes.
        while (sg::backend::vulkan::device_driver_barriers_up() == 0 && !finished.load(std::memory_order_acquire))
            std::this_thread::yield();
        CHECK(!finished.load(std::memory_order_acquire));

        opener->release();
        creator.join();
        CHECK(created.load(std::memory_order_acquire));

        auto const landed = co_await readback.bytes();
        CHECK(landed[255] == byte(7));
        CHECK(stream.is_complete());
        co_await ctx->idle_completion();
    } // every resource of ctx dies before it shuts down

    if (second != nullptr)
        second->shutdown();
    ctx->shutdown();
#else
    SUCCEED("without threads the barrier drives the transfer actors itself, and a gated source would never finish");
    co_return; // still a coroutine without the threaded arm's awaits
#endif
}
