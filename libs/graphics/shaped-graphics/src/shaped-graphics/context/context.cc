#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/stamp.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/pipeline_cache.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr / std::shared_ptr
#include <shaped-graphics/query/gpu_timestamp.hh>

#include <thread>

namespace sg
{
struct context::completion_signals
{
#if CC_HAS_THREADS
    std::thread waiter;
#else
    cc::thread_pump_registration pump;
#endif
    bool is_started = false;
    bool is_stopped = false;   // under the pending lock; set once, by stop_completion_signals or a lost device
    bool is_torn_down = false; // stop_completion_signals has joined the waiter; only the shutting-down thread reads it

    // Strictly increasing under the lock, because a vulkan timeline rejects a host signal that does not raise it.
    cc::mutex<u64> wake_generation = cc::mutex<u64>(0);

    // The targets the waiter last armed, zero where none; under the pending lock.
    // A new target at or past one of these needs no wake, since the waiter passes through it on the way.
    u64 armed_submission = 0;
    u64 armed_epoch = 0;

    // Read without the lock by the pump and by a drain reaching zero, which must stay cheap with nothing outstanding.
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

    // Made here rather than on the first completion: transfer actors read the pointer without the pending lock.
    _completion_signals = std::make_unique<completion_signals>();

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
        bool needs_wake = false;
    };
    auto const m = _pending_completions.lock(
        [&](cc::vector<pending_completion>& pending) -> minted
        {
            if (is_device_lost())
                return {.node = make_failed_completion(device_loss_reason())};
            if (_completion_signals != nullptr && _completion_signals->is_stopped)
                return {.node = make_failed_completion("the context shut down before this completion settled")};

            for (auto const& p : pending)
                if (p.target == target && p.kind == kind)
                    return {.node = p.node};

            auto fresh = cc::make_async_manual<cc::unit>();
            pending.push_back({.target = target, .kind = kind, .node = fresh});
            ensure_completion_signals(pending);
            auto& s = *_completion_signals;
            s.has_pending.store(true, cc::memory_order_release);

            auto const armed = kind == completion_kind::epoch      ? s.armed_epoch
                             : kind == completion_kind::submission ? s.armed_submission
                                                                   : 0;
            auto const needs_wake = kind != completion_kind::transfers_drained
                                 && target != u64(submission_token::not_submitted) && (armed == 0 || target < armed);
            return {.node = fresh, .is_new = true, .needs_wake = needs_wake};
        });

    if (m.is_new)
    {
        if (m.needs_wake)
            wake_completion_signals();
        // The condition may have come true between the caller's check and the mint, and a drain reaching zero or a fence
        // passing in that window found nothing to settle — so look once more now that the node is where they look.
        settle_due_completions();
    }
    return m.node;
}

void context::ensure_completion_signals(cc::vector<pending_completion> const& pending)
{
    (void)pending; // proof the caller holds the lock
    auto& s = *_completion_signals;
    if (s.is_started)
        return;
    s.is_started = true;

#if CC_HAS_THREADS
    s.waiter = std::thread([this] { run_completion_signal_waiter(); });
#else
    // No thread to park on the GPU, so whoever sweeps the pumps does it — only once nothing else could progress.
    s.pump = cc::register_thread_pump([this] { return pump_completion_signals(); });
#endif
}

void context::wake_completion_signals()
{
#if !CC_HAS_THREADS
    // Nothing parks on a wake without threads: the pump's GPU wait is the only one, and a raised wake would end it at once
    // on every sweep from then on.
    return;
#else
    auto* const s = _completion_signals.get();
    if (s == nullptr)
        return;
    s->wake_generation.lock(
        [&](u64& generation)
        {
            ++generation;
            wake_completion_signal(generation);
        });
#endif
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
            return out;
        });

    // A lost device jumps every dx12 fence to its maximum, which would read as success here.
    for (auto const& node : due)
        if (lost)
            node->push_error(cc::async_error::make_error(cc::any_error(device_loss_reason())));
        else
            node->push_value(cc::unit{});
}

void context::run_completion_signal_waiter()
{
    auto& s = *_completion_signals;
    while (true)
    {
        // The generation is read before the targets, so a wake that adds a target after this line is never missed.
        auto const generation = s.wake_generation.lock([](u64& g) { return g; });
        auto const [stop, targets] = _pending_completions.lock(
            [&](cc::vector<pending_completion>& pending) -> cc::pair<bool, completion_targets>
            {
                auto t = completion_targets();
                for (auto const& p : pending)
                {
                    if (p.kind == completion_kind::epoch)
                        t.epoch = t.epoch == 0 ? p.target : cc::min(t.epoch, p.target);
                    else if (p.kind == completion_kind::submission && p.target != u64(submission_token::not_submitted))
                        t.submission = t.submission == 0 ? p.target : cc::min(t.submission, p.target);
                }
                s.armed_submission = t.submission;
                s.armed_epoch = t.epoch;
                return {s.is_stopped, t};
            });
        if (stop)
            return;

        wait_for_completion_signal(targets.submission, targets.epoch, generation);
        settle_due_completions();

        // A lost device settles everything as an error above, and its signals may never fire again.
        if (is_device_lost())
        {
            _pending_completions.lock([&](cc::vector<pending_completion>&) { s.is_stopped = true; });
            settle_due_completions();
            return;
        }
    }
}

bool context::pump_completion_signals()
{
    auto& s = *_completion_signals;
    if (!s.has_pending.load(cc::memory_order_acquire))
        return false;

    // Only what the GPU has been handed can signal: the open epoch closes on an advance, and the thread that would
    // advance is the one sweeping here, so parking on it would never return.
    auto const open_epoch = u64(current_epoch());
    auto const targets = _pending_completions.lock(
        [&](cc::vector<pending_completion>& pending)
        {
            auto t = completion_targets();
            for (auto const& p : pending)
            {
                if (p.kind == completion_kind::epoch && p.target < open_epoch)
                    t.epoch = t.epoch == 0 ? p.target : cc::min(t.epoch, p.target);
                else if (p.kind == completion_kind::submission && p.target != u64(submission_token::not_submitted))
                    t.submission = t.submission == 0 ? p.target : cc::min(t.submission, p.target);
            }
            return t;
        });

    settle_due_completions();
    if (targets.submission == 0 && targets.epoch == 0)
        return false; // nothing the GPU could signal: drains settle from their actors, open epochs after an advance

    // A GPU target may itself wait on a copy only a sibling actor signals, so every sibling runs before this parks.
    if (cc::thread_pump_all())
        return true;

    wait_for_completion_signal(targets.submission, targets.epoch, 0);
    settle_due_completions();
    return true;
}

void context::stop_completion_signals()
{
    auto* const s = _completion_signals.get();
    if (s == nullptr || s->is_torn_down)
        return;
    s->is_torn_down = true;

    _pending_completions.lock([&](cc::vector<pending_completion>&) { s->is_stopped = true; });
    wake_completion_signals();
#if CC_HAS_THREADS
    if (s->waiter.joinable())
        s->waiter.join();
#else
    s->pump.reset();
#endif

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

cc::shared_async<cc::unit> context::idle_completion()
{
    // Three things, in block_until_idle's order and for its reasons: the GPU first, since an actor delivers a
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
              "block_until_idle() waits, and this context cannot — read completion off the *_completion() asyncs, or "
              "poll across frames");

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

    stop_completion_signals();

    // Same rule, one layer down: the transient heap is GPU memory from the device about to be destroyed.
    transient.release_heap_at_shutdown();

    // And the same again for the pipeline cache, which holds layouts and pipelines built on that device.
    release_cached_pipelines();

    _is_shut_down = true;
}
} // namespace sg
