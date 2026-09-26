#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/stamp.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread_bound_scheduler.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/pipeline_cache.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr / std::shared_ptr
#include <shaped-graphics/query/gpu_timestamp.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

namespace sg
{
struct context::completion_signals
{
    bool is_stopped = false;   // under the pending lock; set once, by stop_completion_signals or a lost device
    bool is_torn_down = false; // stop_completion_signals has run; only the shutting-down thread reads it

    // The targets last handed to arm_completion_signal, zero where none; under the pending lock.
    u64 armed_submission = 0;
    u64 armed_epoch = 0;

    // Read without the lock by a drain reaching zero, which must stay cheap with nothing outstanding.
    cc::atomic<bool> has_pending = false;
};

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
                       "already submit to avoid the submit");

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

    // Made here rather than on the first completion: transfer actors read the pointer without the pending lock.
    _completion_signals = std::make_unique<completion_signals>();

    for (auto format : accepted_shader_formats)
        _accepted_shader_formats.push_back(format);

    if (threading == thread_model::main_thread)
    {
        CC_ASSERT(cc::current_thread_id() == cc::thread_id::main, "a main_thread context is created on the main "
                                                                  "thread");
        _device_home = &cc::main_thread_scheduler();
    }

    // The scope members only store a back-reference; they don't touch any not-yet-constructed member.
    // Give the built-in cache default in-memory tiers so ctx.cached memoizes out of the box.
    _pipeline_cache->add_default_in_memory_providers();
}

bool context::is_on_device_thread() const
{
    switch (_thread_model)
    {
    case thread_model::main_thread:
        return cc::current_thread_id() == cc::thread_id::main;
    case thread_model::single_threaded:
        return cc::current_thread_id() == _creating_thread;
    case thread_model::multi_threaded:
        return true;
    }
    CC_UNREACHABLE("unknown thread_model");
}

void context::assert_on_device_thread() const
{
    CC_ASSERT(is_on_device_thread(),
              "this context call is bound by the thread model and was made from another thread; "
              "only asyncs, layouts and samplers are free-threaded (see docs/concepts/threading.md)");
}

bool context::accepts_shader_format(shader_format format) const
{
    for (auto accepted : _accepted_shader_formats)
        if (accepted == format)
            return true;
    return false;
}

feature_set context::supported_features() const
{
    auto result = feature_set();
    for (auto const f : k_all_features)
        if (supports(f))
            result.set(f);
    return result;
}

feature_set context::missing_features(compiled_shader const& shader) const
{
    if (!shader.required_features.has_value())
        return {};
    return shader.required_features.value().without(supported_features());
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


namespace
{
cc::shared_async<cc::unit> make_failed_completion(cc::string_view why)
{
    auto node = cc::make_async_manual<cc::unit>();
    node->push_error(cc::async_error::make_error(cc::any_error(cc::string(why))));
    return node;
}

/// The lowest outstanding submission and epoch targets, zero where none waits.
/// A `not_submitted` submission never arrives, so it is no target at all.
struct completion_targets
{
    u64 submission = 0;
    u64 epoch = 0;
};
} // namespace

cc::shared_async<cc::unit const> context::completion_for(u64 target, completion_kind kind)
{
    struct minted
    {
        cc::shared_async<cc::unit const> node;
        bool is_new = false;
    };
    auto const m = _pending_completions.lock(
        [&](cc::vector<pending_completion>& pending) -> minted
        {
            if (is_device_lost())
                return {.node = make_failed_completion(device_loss_reason())};
            if (_completion_signals->is_stopped)
                return {.node = make_failed_completion("the context shut down before this completion settled")};

            for (auto const& p : pending)
                if (p.target == target && p.kind == kind)
                    return {.node = p.node};

            auto fresh = cc::make_async_manual<cc::unit>();
            pending.push_back({.target = target, .kind = kind, .node = fresh});
            _completion_signals->has_pending.store(true, cc::memory_order_release);
            rearm_completion_signal(pending);
            return {.node = fresh, .is_new = true};
        });

    // The condition may have come true between the caller's check and the mint, and a drain reaching zero or a fence
    // passing in that window found nothing to settle — so look once more now that the node is where they look.
    if (m.is_new)
        settle_due_completions();
    return m.node;
}

void context::rearm_completion_signal(cc::vector<pending_completion> const& pending)
{
    auto t = completion_targets();
    for (auto const& p : pending)
    {
        if (p.kind == completion_kind::epoch)
            t.epoch = t.epoch == 0 ? p.target : cc::min(t.epoch, p.target);
        else if (p.kind == completion_kind::submission && p.target != u64(submission_token::not_submitted))
            t.submission = t.submission == 0 ? p.target : cc::min(t.submission, p.target);
    }

    auto& s = *_completion_signals;
    if (t.submission == s.armed_submission && t.epoch == s.armed_epoch)
        return;
    s.armed_submission = t.submission;
    s.armed_epoch = t.epoch;
    arm_completion_signal(t.submission, t.epoch);
}

void context::settle_due_completions()
{
    auto* const signals = _completion_signals.get();
    if (signals == nullptr || !signals->has_pending.load(cc::memory_order_acquire))
        return;

    // Taken out under the lock and pushed outside it: pushing resumes whoever depended on the node, and a dependent
    // that reaches back in here would deadlock on a mutex this thread still holds.
    auto const lost = is_device_lost();
    auto due = _pending_completions.lock(
        [&](cc::vector<pending_completion>& pending)
        {
            auto out = cc::vector<cc::shared_async<cc::unit>>();
            auto const completed = u64(completed_epoch());
            for (auto i = pending.size(); i > 0; --i)
            {
                auto& p = pending[i - 1];
                auto reached = lost;
                if (!reached)
                {
                    switch (p.kind)
                    {
                    case completion_kind::epoch:
                        reached = p.target <= completed;
                        break;
                    case completion_kind::submission:
                        reached = is_submission_complete(submission_token(p.target));
                        break;
                    case completion_kind::transfers_drained:
                        reached = are_transfers_drained();
                        break;
                    }
                }
                if (!reached)
                    continue;
                out.push_back(cc::move(p.node));
                pending.remove_at_unordered(i - 1);
            }
            signals->has_pending.store(!pending.empty(), cc::memory_order_release);
            if (!out.empty() && !signals->is_stopped)
                rearm_completion_signal(pending);
            return out;
        });

    // A lost device jumps every dx12 fence to its maximum, which would read as success here.
    for (auto const& node : due)
        if (lost)
            node->push_error(cc::async_error::make_error(cc::any_error(device_loss_reason())));
        else
            node->push_value(cc::unit{});
}

void context::stop_completion_signals()
{
    auto* const s = _completion_signals.get();
    if (s == nullptr || s->is_torn_down)
        return;
    s->is_torn_down = true;

    _pending_completions.lock([&](cc::vector<pending_completion>&) { s->is_stopped = true; });

    // What is due settles as a value; everything else fails rather than parking its dependents for the process's lifetime.
    settle_due_completions();
    auto const outstanding = _pending_completions.lock(
        [&](cc::vector<pending_completion>& pending)
        {
            auto out = cc::move(pending);
            pending.clear();
            s->has_pending.store(false, cc::memory_order_release);
            return out;
        });
    for (auto const& p : outstanding)
        p.node->push_error(cc::async_error::make_error(cc::any_error("the context shut down before this completion "
                                                                     "settled")));
}

void impl::notify_transfer_drained(context& ctx)
{
    ctx.settle_due_completions();
}

cc::shared_async<cc::unit const> context::epoch_completion(epoch e)
{
    if (u64(e) <= u64(completed_epoch()))
        return make_ready_completion();
    return completion_for(u64(e), completion_kind::epoch);
}

cc::shared_async<cc::unit const> context::submission_completion(submission_token token)
{
    // not_submitted is the one target that never arrives, so it gets a node nothing will ever push — which is what the
    // poll already reports, rather than a ready node claiming work that was never recorded had finished.
    if (token != submission_token::not_submitted && is_submission_complete(token))
        return make_ready_completion();
    return completion_for(u64(token), completion_kind::submission);
}

cc::shared_async<cc::unit const> context::transfers_drained_completion()
{
    if (are_transfers_drained())
        return make_ready_completion();
    return completion_for(0, completion_kind::transfers_drained);
}

namespace
{
/// `node`, homed to the device's home where the context has one, since its steps retire epochs and so touch the device.
[[nodiscard]] cc::shared_async<cc::unit> on_device_home(context const& ctx, cc::shared_async<cc::unit> node)
{
    if (auto* const home = ctx.device_home())
        (void)node->try_home_cold(*home);
    return node;
}
} // namespace

cc::shared_async<cc::unit> context::idle_completion()
{
    return on_device_home(*this, idle_completion_steps());
}

cc::shared_async<cc::unit> context::epochs_in_flight_completion(int allowed_in_flight)
{
    CC_ASSERT(allowed_in_flight >= 0, "allowed_in_flight must be non-negative");
    return on_device_home(*this, epochs_in_flight_steps(allowed_in_flight));
}

cc::shared_async<cc::unit> context::idle_completion_steps()
{
    // Three things, in drain_at_shutdown's order and for its reasons: the GPU first, since an actor delivers a
    // download only after the GPU wrote it, then the actors, then the epochs the submission timeline does not cover.
    if (auto const last = last_issued_submission(); last != submission_token::not_submitted)
        co_await submission_completion(last);
    co_await transfers_drained_completion();

    // Waiting on the newest closed epoch covers every older one, since a fence only rises.
    process_completed_epochs();
    while (in_flight_epoch_count() > 0)
    {
        co_await epoch_completion(epoch(u64(current_epoch()) - 1));
        process_completed_epochs();
    }
    co_return;
}

cc::shared_async<cc::unit> context::epochs_in_flight_steps(int allowed_in_flight)
{
    // After a retire, the oldest epoch still in flight is the one past the newest the GPU finished.
    process_completed_epochs();
    while (in_flight_epoch_count() > allowed_in_flight)
    {
        // The GPU may have finished every closed epoch since the retire above, which makes the next one the open epoch.
        // Nothing signals that before an advance, so it is retired again rather than waited on.
        auto const next = u64(completed_epoch()) + 1;
        if (next < u64(current_epoch()))
            co_await epoch_completion(epoch(next));
        process_completed_epochs();
    }
    co_return;
}

cc::shared_async<compute_pipeline_handle> context::create_compute_pipeline_async(compute_pipeline_description const& desc,
                                                                                 lifetime_scope scope)
{
    auto build = [this, d = compute_pipeline_description(desc),
                  scope](cc::async_context<compute_pipeline_handle>& actx) -> cc::async_step_status
    {
        auto res = try_create_compute_pipeline(d, scope);
        if (res.has_error())
            return actx.error(cc::move(res.error()));
        return actx.success(cc::move(res.value()));
    };
    if (_device_home != nullptr)
        return cc::make_async_scheduled_on<compute_pipeline_handle>(*_device_home, cc::move(build));
    return cc::make_async_scheduled<compute_pipeline_handle>(cc::move(build));
}

cc::shared_async<raster_pipeline_handle> context::create_raster_pipeline_async(raster_pipeline_description const& desc,
                                                                               lifetime_scope scope)
{
    auto build = [this, d = raster_pipeline_description(desc),
                  scope](cc::async_context<raster_pipeline_handle>& actx) -> cc::async_step_status
    {
        auto res = try_create_raster_pipeline(d, scope);
        if (res.has_error())
            return actx.error(cc::move(res.error()));
        return actx.success(cc::move(res.value()));
    };
    if (_device_home != nullptr)
        return cc::make_async_scheduled_on<raster_pipeline_handle>(*_device_home, cc::move(build));
    return cc::make_async_scheduled<raster_pipeline_handle>(cc::move(build));
}

void context::drain_at_shutdown()
{
    // Three things, in this order, and the order is the point.
    // An actor delivers a download's bytes only after the GPU finished writing them, so draining the actors first
    // would let a copy land behind us.
    block_until_submissions_complete();
    block_until_transfers_drained();

    // And the epoch fence last, which the submission timeline does NOT cover: an epoch signals after the work it
    // gates, so everything submitted can be done while the epoch that owns it has not retired — and its command
    // allocators, its staged deletions and its finalizers would still be outstanding.
    process_completed_epochs();
    while (in_flight_epoch_count() > 0)
        wait_for_next_inflight_epoch(); // retires as it goes, so this terminates
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

    stop_completion_signals();

    // Same rule, one layer down: the transient heap is GPU memory from the device about to be destroyed.
    transient.release_heap_at_shutdown();

    // And the same again for the pipeline cache, which holds layouts and pipelines built on that device.
    release_cached_pipelines();

    _is_shut_down = true;
}
} // namespace sg
