#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/stamp.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/pipeline_cache.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr / std::shared_ptr
#include <shaped-graphics/query/gpu_timestamp.hh>

namespace sg
{
std::unique_ptr<command_list> context::create_command_list()
{
    auto r = try_create_command_list();
    if (r.has_value())
        return cc::move(r.value());

    // Command-list creation only fails on device loss (backend already marked it) or an internal bug.
    if (_device_lost)
        throw device_lost_exception(_device_loss_reason);

    CC_UNREACHABLE("create_command_list failed on a healthy device — internal error");
}

swapchain_handle context::create_swapchain(swapchain_description const& desc)
{
    auto r = try_create_swapchain(desc);
    if (r.has_value())
        return cc::move(r.value());

    if (_device_lost)
        throw device_lost_exception(_device_loss_reason);
    throw swapchain_creation_exception(r.error());
}

submission_token context::submit_command_list_and_present(swapchain& sc, std::unique_ptr<command_list> cmd)
{
    // Fold the back-buffer's present-layout transition into the caller's command list, submit, then present.
    sc.record_present_transition(*cmd);
    auto const token = submit_command_list(cc::move(cmd));
    sc.present();
    return token;
}

void context::mark_device_lost(cc::string reason)
{
    // Sticky: the first observed reason wins; later observations don't overwrite it.
    if (_device_lost)
        return;
    _device_lost = true;
    _device_loss_reason = cc::move(reason);
}

context::context(backend_kind backend, thread_model threading, cc::span<shader_format const> accepted_shader_formats)
  : persistent(*this),
    transient(*this),
    upload(*this),
    download(*this),
    stream(*this),
    uncached(*this),
    cached(*this),
    _backend(backend),
    _thread_model(threading),
    _pipeline_cache(std::make_unique<pipeline_cache>())
{
    CC_ASSERT(!accepted_shader_formats.empty(), "a context must accept at least one shader format");
    for (auto format : accepted_shader_formats)
        _accepted_shader_formats.push_back(format);

    // The scope members only store a back-reference; they don't touch any not-yet-constructed member.
    // Give the built-in cache default in-memory tiers so ctx.cached memoizes out of the box.
    _pipeline_cache->add_default_in_memory_providers();
}

bool context::accepts_shader_format(shader_format format) const
{
    for (auto accepted : _accepted_shader_formats)
        if (accepted == format)
            return true;
    return false;
}

pipeline_cache& context::pipeline_cache_ref()
{
    return *_pipeline_cache;
}

cc::optional<u64> context::wait_for_ticks(gpu_timestamp const& timestamp)
{
    if (timestamp._heap_future == nullptr)
        return {};
    // Its heap is delivered by the readback actor like any other download, so this blocks on the actor exactly as wait_for(future) does.
    // It needs the same pump to make progress without threads.
    drive_transfers_until_ready(*timestamp._heap_future);
    auto const data = timestamp._heap_future->wait_get_data();
    if (!data.has_value())
        return {};
    CC_ASSERT(timestamp._index < data.value().size(), "timestamp index out of range for its heap download");
    return data.value()[timestamp._index];
}

cc::optional<double> context::wait_for_seconds(gpu_timestamp const& timestamp)
{
    auto const ticks = wait_for_ticks(timestamp);
    if (!ticks.has_value())
        return {};
    return double(ticks.value()) * timestamp._tick_to_seconds;
}

namespace
{
/// The GPU section of a recording's stamp.
///
/// **The FIRST context's adapter**, held in a string that is written once and never rewritten.
/// A stamp provider hands back a span, so the bytes have to outlive the call, and a program creating several contexts
/// would otherwise be rewriting a buffer a recorder is reading.
/// One adapter is the overwhelmingly common case, and describing the first one is better than describing none.
cc::string g_gpu_section;
cc::atomic<bool> g_gpu_section_written = {false};

cc::span<byte const> gpu_stamp_provider(cc::rec::stamp_moment)
{
    // Identical at open and at close: an adapter cannot change under a running context.
    return cc::span<byte const>(reinterpret_cast<byte const*>(g_gpu_section.data()), g_gpu_section.size());
}
} // namespace

void context::set_adapter_info(adapter_info info)
{
    _adapter = cc::move(info);

    auto expected = false;
    if (!g_gpu_section_written.compare_exchange_strong(expected, true, cc::memory_order_acq_rel))
        return;

    g_gpu_section.appendf("gpu.name={}\n", _adapter.name);
    g_gpu_section.appendf("gpu.vendor_id={}\n", _adapter.vendor_id);
    g_gpu_section.appendf("gpu.device_id={}\n", _adapter.device_id);
    g_gpu_section.appendf("gpu.driver_version={}\n", _adapter.driver_version);
    g_gpu_section.appendf("gpu.is_software={}\n", _adapter.is_software ? 1 : 0);
    if (_adapter.dedicated_video_memory_bytes.has_value())
        g_gpu_section.appendf("gpu.dedicated_video_memory_bytes={}\n", _adapter.dedicated_video_memory_bytes.value());

    // cc knows nothing about GPUs; this is the whole of how a recording learns which one is in the machine.
    (void)cc::rec::register_stamp_contributor("sg.gpu", gpu_stamp_provider);
}

cc::result<gpu_memory_usage> context::query_gpu_memory() const
{
    return cc::error("this backend does not report GPU memory");
}

cc::result<gpu_counters> context::read_gpu_counters() const
{
    // Not an empty counter set, which a sampler would difference into a confident 0% on a pinned GPU.
    return cc::error("this backend does not report GPU busy counters");
}

context::~context()
{
    // The backend destructor calls shutdown() before this base destructor runs.
    // Reaching here not shut down means a lifetime/order bug — e.g. a context torn down through a path that skipped it.
    CC_ASSERT(_is_shut_down, "context must be shut down before destruction");
}

void context::shutdown()
{
    // A base context has no backend resources of its own; a backend overrides this to release its device/queue/tracking and duplicate this idempotent flag flip.
    // Routine instances are released at the top of each backend's shutdown (see routines.clear() there), before its resource systems are torn down.
    // A routine's cached GPU state must not outlive the device it was built on.
    routines.clear();
    _is_shut_down = true;
}
} // namespace sg
