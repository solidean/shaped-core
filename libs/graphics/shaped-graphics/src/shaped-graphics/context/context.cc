#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
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

cc::optional<submission_token> context::prepare_texture_for_async(raw_texture_handle const& texture,
                                                                  subresource_range const& range,
                                                                  async_direction direction)
{
    CC_ASSERT(texture != nullptr, "prepare_texture_for_async: texture is null");

    auto const required = async_ready_layout(direction);
    if (current_texture_layout(texture, range) == required)
        return {};

    if (texture->claim_async_fixup_warning())
        CC_LOG_WARNING("an async transfer found a texture in a layout its transfer queue cannot use, so a command "
                       "list holding one transition was submitted for it. Record cmd.prepare_for_async on a list you "
                       "are already building, or create the texture with texture_description::initial_layout set, to "
                       "avoid the submit");

    auto cmd = create_command_list();
    cmd->ensure_layout(texture, required, range);
    return submit_command_list(cc::move(cmd));
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
    _device_loss_reason = reason;

    // And onto the deferred channel, so a frame loop that drains errors sees it without polling is_device_lost().
    // Once, because the flag above is sticky — a caller draining every frame would otherwise get it every frame.
    report_device_error({.kind = device_error_kind::device_lost, .message = cc::move(reason)});
}

context::context(backend_kind backend, thread_model threading, cc::span<shader_format const> accepted_shader_formats)
  : persistent(*this),
    transient(*this),
    upload(*this),
    download(*this),
    stream(*this),
    uncached(*this),
    cached(*this),
    routines(*this),
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

void context::release_cached_pipelines()
{
    _pipeline_cache->release_at_shutdown();
}

pipeline_cache& context::pipeline_cache_ref()
{
    return *_pipeline_cache;
}

cc::vector<device_error> context::take_pending_errors()
{
    return _pending_errors.lock(
        [](cc::vector<device_error>& pending)
        {
            auto out = cc::move(pending);
            pending.clear();
            return out;
        });
}

void context::report_device_error(device_error error)
{
    _pending_errors.lock([&](cc::vector<device_error>& pending) { pending.push_back(cc::move(error)); });
}

void context::process_completed_epochs()
{
    retire_completed_epochs();
    settle_due_completions();
}

bool context::try_advance_epoch(int allowed_in_flight)
{
    CC_ASSERT(allowed_in_flight >= 0, "allowed_in_flight must be non-negative");

    // Retire first, and only then decide: an epoch the GPU finished but nobody has reclaimed still counts as in flight,
    // so a caller that skipped this would decline against depth that is no longer there.
    process_completed_epochs();
    if (in_flight_epoch_count() > allowed_in_flight)
        return false;

    advance_epoch();
    return true;
}

cc::shared_async<cc::unit const> context::completion_for(u64 target, bool is_submission)
{
    return _pending_completions.lock(
        [&](cc::vector<pending_completion>& pending) -> cc::shared_async<cc::unit const>
        {
            for (auto const& p : pending)
                if (p.target == target && p.is_submission == is_submission)
                    return p.node;

            auto node = cc::make_async_manual<cc::unit>();
            pending.push_back({.target = target, .is_submission = is_submission, .node = node});
            return node;
        });
}

void context::settle_due_completions()
{
    // Taken out under the lock and pushed outside it: pushing resumes whoever depended on the node, and a dependent
    // that reaches back in here would deadlock on a mutex this thread still holds.
    auto due = _pending_completions.lock(
        [this](cc::vector<pending_completion>& pending)
        {
            auto out = cc::vector<cc::shared_async<cc::unit>>();
            auto const completed = u64(completed_epoch());
            for (auto i = pending.size(); i > 0; --i)
            {
                auto& p = pending[i - 1];
                auto const reached
                    = p.is_submission ? is_submission_complete(submission_token(p.target)) : p.target <= completed;
                if (!reached)
                    continue;
                out.push_back(cc::move(p.node));
                pending.remove_at_unordered(i - 1);
            }
            return out;
        });

    for (auto const& node : due)
        node->push_value(cc::unit{});
}

cc::shared_async<cc::unit const> context::epoch_completion(epoch e)
{
    if (u64(e) <= u64(completed_epoch()))
        return make_ready_completion();
    return completion_for(u64(e), false);
}

cc::shared_async<cc::unit const> context::submission_completion(submission_token token)
{
    // not_submitted is the one target that never arrives, so it gets a node nothing will ever push — which is what the
    // poll already reports, rather than a ready node claiming work that was never recorded had finished.
    if (token != submission_token::not_submitted && is_submission_complete(token))
        return make_ready_completion();
    return completion_for(u64(token), true);
}

void context::block_until_epochs_in_flight(int allowed_in_flight)
{
    CC_ASSERT(execution() == execution_model::may_block,
              "block_until_epochs_in_flight() waits, and this context cannot — bound the depth with "
              "try_advance_epoch() instead");
    CC_ASSERT(allowed_in_flight >= 0, "allowed_in_flight must be non-negative");

    // Retire before waiting: an epoch the GPU already finished still counts as in flight until someone reclaims it,
    // so a caller that skipped this would park against depth that is no longer there.
    process_completed_epochs();
    while (in_flight_epoch_count() > allowed_in_flight)
        wait_for_next_inflight_epoch(); // retires as it goes, so this terminates
}

void context::block_until_idle()
{
    CC_ASSERT(execution() == execution_model::may_block,
              "block_until_idle() is the one call in sg that waits, and this context cannot — read completion off the "
              "*_completion() asyncs, or poll across frames");

    // Three things, in this order, and the order is the point.
    // An actor delivers a download's bytes only after the GPU finished writing them, so draining the actors first
    // would let a copy land behind us.
    block_until_submissions_complete();
    block_until_transfers_drained();

    // And the epoch fence last, which the submission timeline does NOT cover: an epoch signals after the work it
    // gates, so everything submitted can be done while the epoch that owns it has not retired — and its command
    // allocators, its staged deletions and its finalizers would still be outstanding.
    block_until_epochs_in_flight(0);
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

    // Same rule, one layer down: the transient heap is GPU memory from the device about to be destroyed.
    transient.release_heap_at_shutdown();

    // And the same again for the pipeline cache, which holds layouts and pipelines built on that device.
    release_cached_pipelines();

    _is_shut_down = true;
}
} // namespace sg
