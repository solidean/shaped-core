#include "dx12-test-common.hh"

#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/stamp.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace dx12 = sg::backend::dx12;

// Which GPU is in the machine is not something a test can know, so nothing here asserts a size.
// What it pins is that the two memory figures are different scales and both coherent, and that an unimplemented query
// refuses rather than reporting an idle GPU.

TEST("sg dx12 - the adapter reports the memory on the board")
{
    auto handle = dx12::make_hardware_context();
    if (handle == nullptr)
        SKIP("no hardware adapter on this machine");

    auto const& adapter = handle->adapter();
    REQUIRE(adapter.dedicated_video_memory_bytes.has_value());

    // Zero is a real answer for an integrated GPU, so the assertion is only that it is not negative.
    CHECK(adapter.dedicated_video_memory_bytes.value() >= 0);
}

TEST("sg dx12 - the memory budget is what this process may use, not what the board has")
{
    auto handle = dx12::make_hardware_context();
    if (handle == nullptr)
        SKIP("no hardware adapter on this machine");

    auto const memory = handle->query_gpu_memory();
    if (memory.has_error())
        SKIP("this runtime has no IDXGIAdapter3");

    CHECK(memory.value().budget_bytes > 0);
    CHECK(memory.value().current_usage_bytes >= 0);

    // A live context has allocated something, so usage is real rather than a zero-filled struct.
    CHECK(memory.value().current_usage_bytes > 0);

    // The budget shrinks as other processes take memory, so it never exceeds the board — the two are related but not
    // interchangeable, which is exactly what a dashboard gets wrong.
    auto const& board = handle->adapter().dedicated_video_memory_bytes;
    if (board.has_value() && board.value() > 0)
        CHECK(memory.value().budget_bytes <= board.value());
}

TEST("sg dx12 - GPU busy counters are monotone and named per engine")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);

    auto first = handle->read_gpu_counters();
    if (first.has_error())
        SKIP("the GPU Engine performance counters are unavailable here");

    CHECK(!first.value().engines.empty());
    for (auto const& e : first.value().engines)
    {
        CHECK(!e.engine.empty());
        CHECK(e.busy_secs >= 0);
    }

    auto second = handle->read_gpu_counters();
    REQUIRE(second.has_value());

    // Matched by name, because the engine set can differ between two readings and pairing by index would difference
    // two unrelated counters.
    for (auto const& before : first.value().engines)
        for (auto const& after : second.value().engines)
            if (cc::string_view(before.engine) == cc::string_view(after.engine))
                CHECK(after.busy_secs >= before.busy_secs);
}

TEST("sg dx12 - a sampled GPU load is the busiest engine, in range")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);

    if (!sg::gpu_load_sampler::is_supported(*handle))
        SKIP("the GPU Engine performance counters are unavailable here");

    auto sampler = sg::gpu_load_sampler(*handle);
    cc::this_thread_sleep_secs(0.05);

    auto const load = sampler.sample();
    REQUIRE(load.has_value());

    CHECK(load.value().interval_secs > 0);
    CHECK(load.value().total >= 0.0f);
    CHECK(load.value().total <= 1.0f);

    // The total is the MAX across engines, never the sum: a device with four engines would otherwise report over 100%.
    auto busiest = 0.0f;
    for (auto const& e : load.value().per_engine)
    {
        CHECK(e.busy >= 0.0f);
        CHECK(e.busy <= 1.0f);
        busiest = e.busy > busiest ? e.busy : busiest;
    }
    CHECK(load.value().total == busiest);
}

TEST("sg dx12 - a software adapter still answers coherently")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);

    // WARP has no board memory, and the query must say something true rather than crash or invent a budget.
    auto const& adapter = handle->adapter();
    CHECK(adapter.is_software);
    if (adapter.dedicated_video_memory_bytes.has_value())
        CHECK(adapter.dedicated_video_memory_bytes.value() >= 0);

    if (auto const memory = handle->query_gpu_memory(); memory.has_value())
        CHECK(memory.value().current_usage_bytes >= 0);
}

TEST("sg dx12 - print the GPU metrics", nx::config::manual)
{
    // Never swept: the numbers are whatever this machine's GPU is doing, and the point is that a human reads them.
    // A test can assert a budget is positive; only a person notices it is implausible.
    auto handle = dx12::make_hardware_context();
    if (handle == nullptr)
        SKIP("no hardware adapter on this machine");

    auto const& adapter = handle->adapter();
    auto const to_mib = [](i64 bytes) { return double(bytes) / (1024.0 * 1024.0); };

    cc::println("");
    cc::println("  adapter        {}", adapter.name);
    cc::println("  vendor/device  {:#x} / {:#x}", adapter.vendor_id, adapter.device_id);
    cc::println("  board memory   {:.0f} MiB", adapter.dedicated_video_memory_bytes.has_value()
                                                   ? to_mib(adapter.dedicated_video_memory_bytes.value())
                                                   : 0.0);

    if (auto const memory = handle->query_gpu_memory(); memory.has_value())
        cc::println("  process budget {:.0f} MiB, using {:.0f} MiB", to_mib(memory.value().budget_bytes),
                    to_mib(memory.value().current_usage_bytes));
    else
        cc::println("  process budget (unavailable: {})", memory.error().to_string());

    // The cumulative counters, because 0% on an idle GPU looks identical to counters that are always zero.
    if (auto const counters = handle->read_gpu_counters(); counters.has_value())
        for (auto const& e : counters.value().engines)
            cc::println("  engine {:<10} {:.1f} s busy since boot", e.engine, e.busy_secs);

    auto sampler = sg::gpu_load_sampler(*handle);
    cc::this_thread_sleep_secs(0.5);
    if (auto const load = sampler.sample(); load.has_value())
    {
        cc::println("  load           {:.0f}% over {:.2f} s (busiest engine)", 100.0f * load.value().total,
                    load.value().interval_secs);
        for (auto const& e : load.value().per_engine)
            if (e.busy > 0.0f)
                cc::println("    {:<12} {:.1f}%", e.engine, 100.0f * e.busy);
    }
    else
    {
        cc::println("  load           (unavailable: {})", load.error().to_string());
    }
}

TEST("sg dx12 - a recording is stamped with the GPU", nx::config::exclusive())
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);

    auto listener = cc::rec::recording_listener();
    {
        auto const registration = cc::rec::register_listener(listener);
        cc::rec::emit_stamp(cc::rec::stamp_moment::open);
        cc::rec::flush_blocking();
        cc::rec::unregister_listener(registration);
    }

    // The whole point of the contributor seam: cc has no idea GPUs exist, and the recording still says which one is in
    // the machine.
    auto section = cc::string();
    listener.result().for_each_event(
        [&section](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::stamp && cc::string_view(e.name()) == "sg.gpu")
                section = cc::string(e.payload_as_text());
        });

    CHECK(!section.empty());
    CHECK(cc::string_view(section).contains("gpu.name="));
    CHECK(cc::string_view(section).contains("gpu.vendor_id="));
}
