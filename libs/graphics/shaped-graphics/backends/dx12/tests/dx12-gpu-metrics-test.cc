#include "dx12-test-common.hh"

#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/stamp.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/print.hh>
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

TEST("sg dx12 - GPU load refuses rather than reporting an idle device")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);

    // D3D12 exposes no utilization query, and the Windows route is not implemented yet.
    // A zero here would draw as an idle GPU on a machine that is pinned, which is the failure this refuses to make.
    auto const load = handle->query_gpu_load();
    CHECK(load.has_error());
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

    if (auto const load = handle->query_gpu_load(); load.has_value())
        cc::println("  load           {:.0f}%", 100.0f * load.value().total);
    else
        cc::println("  load           (unavailable: {})", load.error().to_string());
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
