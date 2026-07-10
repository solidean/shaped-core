#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/allocation_info.hh>
#include <shaped-graphics/backend/command_list_slot.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_command_allocator_pool.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_descriptor_heap.hh>
#include <shaped-graphics/backends/dx12/dx12_download_async.hh>
#include <shaped-graphics/backends/dx12/dx12_download_inline.hh>
#include <shaped-graphics/backends/dx12/dx12_epoch.hh>
#include <shaped-graphics/backends/dx12/dx12_memory_heap.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_upload_async.hh>
#include <shaped-graphics/backends/dx12/dx12_upload_inline.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/context.hh>

#include <atomic>
#include <memory>

namespace sg::backend::dx12
{
/// Creation config for the dx12 context. The flags are independent.
struct dx12_config
{
    /// Enable the D3D12 debug/validation layer. Best-effort — skipped if it isn't installed.
    bool enable_debug_layer = false;

    /// Use the WARP software adapter instead of a hardware GPU. Runs headless (good for CI).
    bool use_warp = false;

    /// Capacity of the inline UPLOAD ring buffer, in bytes. Bounds the per-epoch inline upload volume.
    cc::isize upload_ring_bytes = cc::isize(16) * 1024 * 1024;

    /// Capacity of the inline READBACK ring buffer, in bytes. Bounds the in-flight inline download volume.
    cc::isize download_ring_bytes = cc::isize(16) * 1024 * 1024;

    /// Size of one async-upload staging window, in bytes. The staging buffer is triple-buffered (three
    /// of these), so CPU memcpy and GPU copy overlap; an upload larger than a window packs across
    /// successive windows. Bigger windows amortize submits; smaller ones cut latency and memory.
    cc::isize async_upload_window_bytes = cc::isize(16) * 1024 * 1024;

    /// Size of one async-download (readback) staging window, in bytes. Triple-buffered like the upload
    /// window; a read larger than a window packs across successive windows. Bigger windows amortize
    /// submits; smaller ones cut latency and memory.
    cc::isize async_download_window_bytes = cc::isize(16) * 1024 * 1024;

    /// Total descriptors in the shader-visible CBV/SRV/UAV heap binding_groups allocate their tables from.
    int descriptor_heap_capacity = 1 << 16;

    /// Share (0..1) of the descriptor heap reserved for the per-epoch-reclaimed transient ring; the
    /// rest is the persistent bump region.
    float descriptor_transient_fraction = 0.25f;

    /// Total descriptors in the shader-visible SAMPLER heap dynamic samplers are written into. D3D12 caps
    /// a shader-visible sampler heap at 2048; split into transient/persistent by descriptor_transient_fraction.
    int sampler_heap_capacity = 512;
};

/// DirectX 12 implementation of sg::context. The sg::context virtuals are thin forwarders to the
/// backend-typed create_dx12_* methods — prefer those when you hold a dx12_context. Bodies in dx12.cc.
class dx12_context final : public sg::context
{
public:
    // Default-constructs an empty context; create_dx12_context populates the device objects and
    // initializes the inline transfer systems. The COM pointers and fences start null.
    dx12_context()
      : sg::context(sg::backend_kind::dx12, sg::thread_model::multi_threaded),
        _cmd_pool(*this),
        _upload_inline(*this),
        _download_inline(*this),
        _upload_async(*this),
        _download_async(*this)
    {
    }

    ~dx12_context() override { shutdown(); } // runs shutdown() before the base dtor asserts it

    /// Whether this device supports ray tracing (DXR tier >= 1.0). Cached from CheckFeatureSupport at
    /// creation; a build_blas / build_tlas on an unsupported device asserts. Surfaced through
    /// cmd.raytracing.is_supported().
    [[nodiscard]] bool supports_raytracing() const { return _raytracing_tier >= D3D12_RAYTRACING_TIER_1_0; }

    // backend-typed API — prefer these when you already hold a dx12_context

    [[nodiscard]] cc::result<std::unique_ptr<dx12_command_list>> create_dx12_command_list();
    [[nodiscard]] cc::result<dx12_buffer_handle> create_dx12_buffer(cc::isize size_in_bytes,
                                                                    sg::buffer_usage usage,
                                                                    sg::allocation_info const& alloc);
    [[nodiscard]] cc::result<dx12_texture_handle> create_dx12_texture(sg::texture_description const& desc,
                                                                      sg::allocation_info const& alloc);
    [[nodiscard]] cc::result<dx12_memory_heap_handle> create_dx12_memory_heap(cc::isize size_in_bytes);
    sg::submission_token submit_dx12_command_list(std::unique_ptr<dx12_command_list> cmd);
    void drop_dx12_command_list(std::unique_ptr<dx12_command_list> cmd);

    // The drop cleanup on an unsubmitted list (reclaim its allocator/list/slot, discard its recorded
    // downloads, drop the open-list count). Shared by drop_dx12_command_list and the list's destructor
    // auto-drop — do not call directly; go through drop_dx12_command_list.
    void reclaim_unsubmitted_command_list(dx12_command_list& cmd);

    // Bind path — backend-typed creates (no downcasts when you hold a dx12_context). Bodies in dx12_bind.cc.
    // `scope` is persistent-only for now (transient bind-path resources not implemented yet).
    [[nodiscard]] cc::result<dx12_binding_group_layout_handle> create_dx12_binding_group_layout(
        cc::span<sg::binding const> bindings,
        cc::span<sg::named_sampler const> static_samplers,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<dx12_pipeline_layout_handle> create_dx12_pipeline_layout(
        sg::pipeline_layout_description const& desc,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<dx12_compute_pipeline_handle> create_dx12_compute_pipeline(
        sg::compiled_shader const& shader,
        dx12_pipeline_layout_handle layout,
        cc::span<cc::byte const> cached_pipeline,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<dx12_binding_group_handle> create_dx12_binding_group(dx12_binding_group_layout_handle layout,
                                                                                  cc::span<sg::named_view const> views,
                                                                                  cc::span<sg::named_sampler const> samplers,
                                                                                  sg::lifetime_scope scope);

    /// Stages a refcount-zero GPU resource for deferred deletion, attributed to the epoch it dies in
    /// (freed once that epoch retires). Called from ~dx12_buffer; safe to call from any thread.
    void schedule_deferred_deletion(dx12_expiring_resource expiring);

    // sg::context overrides — forward to the backend-typed methods above. A command list is only
    // ever a dx12 one here (backends never mix), so the downcast is sound; a CC_ASSERT'd dynamic_cast
    // guards against a foreign command list slipping in (compiled out in release).

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> try_create_command_list() override
    {
        return note_device_loss_on_error(cc::result<std::unique_ptr<sg::command_list>>(create_dx12_command_list()),
                                         "create_command_list");
    }

    [[nodiscard]] cc::result<sg::raw_buffer_handle> try_create_raw_buffer(cc::isize size_in_bytes,
                                                                          sg::buffer_usage usage,
                                                                          sg::allocation_info const& alloc) override
    {
        return note_device_loss_on_error(
            cc::result<sg::raw_buffer_handle>(create_dx12_buffer(size_in_bytes, usage, alloc)), "create_raw_buffer");
    }

    [[nodiscard]] cc::result<sg::raw_texture_handle> try_create_raw_texture(sg::texture_description const& desc,
                                                                            sg::allocation_info const& alloc) override
    {
        return note_device_loss_on_error(cc::result<sg::raw_texture_handle>(create_dx12_texture(desc, alloc)),
                                         "create_raw_texture");
    }

    [[nodiscard]] cc::result<sg::memory_heap_handle> try_create_memory_heap(cc::isize size_in_bytes) override
    {
        return note_device_loss_on_error(cc::result<sg::memory_heap_handle>(create_dx12_memory_heap(size_in_bytes)),
                                         "create_memory_heap");
    }

    // Bind-path sg::context overrides — thin forwarders (unpack the description / downcast the sg layout
    // handle) to the backend-typed creates above. Bodies in dx12_bind.cc.
    [[nodiscard]] cc::result<sg::binding_group_layout_handle> try_create_binding_group_layout(
        cc::span<sg::binding const> bindings,
        cc::span<sg::named_sampler const> static_samplers,
        sg::lifetime_scope scope) override;
    [[nodiscard]] cc::result<sg::pipeline_layout_handle> try_create_pipeline_layout(
        sg::pipeline_layout_description const& desc,
        sg::lifetime_scope scope) override;
    [[nodiscard]] cc::result<sg::compute_pipeline_handle> try_create_compute_pipeline(
        sg::compute_pipeline_description const& desc,
        sg::lifetime_scope scope) override;
    [[nodiscard]] cc::result<sg::binding_group_handle> try_create_binding_group(sg::binding_group_layout_handle layout,
                                                                                cc::span<sg::named_view const> views,
                                                                                cc::span<sg::named_sampler const> samplers,
                                                                                sg::lifetime_scope scope) override;

    // Device-loss detection (see is_device_lost). Records the sticky loss reason and returns true if the
    // device is removed — either `hr` is a removed/reset code, or the device reports a non-S_OK removed
    // reason. `what` labels the failing op. Call after any HRESULT failure on the device timeline.
private:
    bool note_device_removed_if_lost(HRESULT hr, char const* what)
    {
        HRESULT reason = (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ? hr : S_OK;
        if (reason == S_OK && _device)
            reason = _device->GetDeviceRemovedReason();
        if (reason == S_OK)
            return false;
        mark_device_lost(cc::format("{} (device removed, reason=0x{:08X})", what, cc::u32(reason)));
        return true;
    }

    // Consults GetDeviceRemovedReason when a create returned an error, so a device-loss-during-create is
    // marked before the throwing façade classifies the failure. Passes the result through unchanged.
    template <class T>
    cc::result<T> note_device_loss_on_error(cc::result<T> r, char const* what)
    {
        if (r.has_error())
            note_device_removed_if_lost(S_OK, what);
        return r;
    }

public:
    sg::submission_token submit_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        CC_ASSERT(dynamic_cast<dx12_command_list*>(cmd.get()) != nullptr, "command list is not a dx12 command list");
        return submit_dx12_command_list(
            std::unique_ptr<dx12_command_list>(static_cast<dx12_command_list*>(cmd.release())));
    }

    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        CC_ASSERT(dynamic_cast<dx12_command_list*>(cmd.get()) != nullptr, "command list is not a dx12 command list");
        drop_dx12_command_list(std::unique_ptr<dx12_command_list>(static_cast<dx12_command_list*>(cmd.release())));
    }

    // Reached through ctx.upload — async CPU→GPU buffer streaming on the copy queue. Forwards to the
    // async upload system; later direct-queue lists reading the buffer auto-wait on the copy.
    void async_upload_bytes_to_buffer(sg::raw_buffer_handle buffer,
                                      cc::pinned_data<cc::byte const> data,
                                      cc::isize offset_in_bytes) override
    {
        _upload_async.upload_buffer(cc::move(buffer), cc::move(data), offset_in_bytes);
    }

    void async_upload_bytes_to_texture(sg::raw_texture_handle texture,
                                       cc::pinned_data<cc::byte const> data,
                                       sg::subresource_index const& subresource,
                                       sg::texture_region const& region) override
    {
        _upload_async.upload_texture(cc::move(texture), cc::move(data), subresource, region);
    }

    // Reached through ctx.download — async GPU→CPU buffer readback on the copy queue. Forwards to the async
    // download system; a later direct-queue list writing the buffer auto-waits on the read.
    [[nodiscard]] sg::bytes_future async_download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                                    cc::isize offset_in_bytes,
                                                                    cc::isize size_in_bytes) override
    {
        return _download_async.download_buffer(cc::move(buffer), offset_in_bytes, size_in_bytes);
    }

    [[nodiscard]] sg::bytes_future async_download_bytes_from_texture(sg::raw_texture_handle texture,
                                                                     sg::subresource_index const& subresource,
                                                                     sg::texture_region const& region) override
    {
        return _download_async.download_texture(cc::move(texture), subresource, region);
    }

    // Runtime transfer-resource resizing (reached via ctx.upload / ctx.download). Each records a pending
    // change on the owning system, applied at a later safe point (see the systems + advance_epoch).
    void set_async_upload_window_bytes(cc::isize bytes) override { _upload_async.set_window_bytes(bytes); }
    void set_async_download_window_bytes(cc::isize bytes) override { _download_async.set_window_bytes(bytes); }
    void set_inline_upload_budget(cc::isize bytes) override { _upload_inline.set_budget(bytes); }
    void set_inline_download_budget(cc::isize bytes) override { _download_inline.set_budget(bytes); }

    // Epoch contract — bodies in dx12_epoch.cc. These return sg vocabulary types (no backend-typed
    // variant needed), so the whole body lives in the override.

    [[nodiscard]] sg::epoch current_epoch() const override { return _current_epoch; }
    [[nodiscard]] sg::epoch completed_epoch() const override;
    void advance_epoch(cc::optional<int> allowed_in_flight) override;
    void advance_epoch_and_wait_for_idle() override { advance_epoch(0); }
    void process_completed_epochs() override;
    void wait_for_epoch(sg::epoch e) override;
    void wait_for_next_inflight_epoch() override;
    [[nodiscard]] bool is_submission_complete(sg::submission_token token) const override;

    void shutdown() override;

    ComPtr<IDXGIFactory4> _factory;
    ComPtr<ID3D12Device> _device;
    ComPtr<ID3D12CommandQueue> _queue;

    // DXR support tier, queried once at creation (D3D12_FEATURE_D3D12_OPTIONS5). NOT_SUPPORTED until set.
    D3D12_RAYTRACING_TIER _raytracing_tier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

    // Epoch machinery. The epoch fence is signaled with the epoch value at the end of each epoch;
    // the submission fence is a per-command-list timeline on the same queue.
    ComPtr<ID3D12Fence> _epoch_fence;
    ComPtr<ID3D12Fence> _submission_fence;
    // Epoch-fence waits (wait_for_epoch) create a per-call event so they stay safe to invoke from any
    // thread — no shared wait event to serialize on.

    // Written only by advance (externally synchronized), read concurrently by create/submit/drop.
    sg::epoch _current_epoch = sg::epoch::first;

    // create / submit / drop are thread-safe, so the shared bookkeeping they touch is synchronized:
    //  - _open_command_lists: bumped per create, dropped per submit/drop — a lock-free counter.
    //  - _next_submission: the next completion token; guarded together with the ExecuteCommandLists +
    //    Signal in submit so token order == queue/signal order (out-of-order signals could move the
    //    fence value backwards).
    std::atomic<int> _open_command_lists = 0; // must reach 0 before advance — lists cannot span epochs
    cc::mutex<sg::submission_token> _next_submission{sg::submission_token::first};

    // Hands each open command list a dense access-tracking slot (a backend helper for concurrent
    // recording); acquired at create, released at submit/drop. Each resource separately tracks how many
    // open lists are using it and promotes when its last active slot finalizes. Internally synchronized.
    sg::command_list_slot_allocator _command_list_slots;

    cc::mutex<dx12_epoch_state> _epoch_state;

    // Extra systems — owned by value, constructed with *this. The command-allocator pool recycles
    // command allocators (epoch-gated) and command lists per queue.
    //
    // TODO: more backend systems will grow here as the API expands — a group-fence pool, async
    // upload/download (copy-queue) systems, a query system, a descriptor system, and RTV/DSV
    // allocators. (Inline upload/download already exist below.)
    dx12_command_allocator_pool _cmd_pool;

    // Inline host↔device buffer transfer over UPLOAD / READBACK ring buffers on the direct queue.
    // Initialized (ring buffers mapped, download actor started) in create_dx12_context.
    dx12_upload_inline_system _upload_inline;
    dx12_download_inline_system _download_inline;

    // Async CPU→GPU buffer streaming on the copy queue (reached via ctx.upload). Owns its staging ring +
    // copy actor; initialized in create_dx12_context after the copy queue/fence exist.
    dx12_upload_async_system _upload_async;

    // Async GPU→CPU buffer readback on the copy queue (reached via ctx.download). Owns its readback staging
    // ring + copy actor; initialized in create_dx12_context after the copy queue + download fence exist.
    dx12_download_async_system _download_async;

    // Transient buffers created in the open epoch, registered here so advance_epoch can auto-expire them
    // (their placed storage in ctx.transient's heap is reused by the next epoch). Weak: never keeps a
    // buffer alive. Guarded because create runs on any thread while advance runs on the driver thread.
    cc::mutex<cc::vector<std::weak_ptr<sg::raw_buffer const>>> _transient_expiring;

    // Transient textures created in the open epoch, auto-expired at advance_epoch just like buffers.
    // Dedicated for now (no bump storage to reuse), but expiry still honours the transient contract.
    cc::mutex<cc::vector<std::weak_ptr<sg::raw_texture const>>> _transient_expiring_textures;

    // Shader-visible CBV/SRV/UAV heap binding_groups allocate their descriptor tables from.
    // Initialized in create_dx12_context.
    dx12_descriptor_heap _descriptor_heap;

    // Shader-visible SAMPLER heap dynamic samplers are written into (a separate heap — D3D12 binds one of
    // each type). Same transient/persistent split as _descriptor_heap. Initialized in create_dx12_context.
    dx12_descriptor_heap _sampler_heap;
};
} // namespace sg::backend::dx12

namespace sg
{
/// Creates a dx12-backed sg::context. Returns an error (never asserts) on environment failure —
/// no adapter, device creation refused, etc. Only callers that link the dx12 backend see it.
[[nodiscard]] cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config = {});
} // namespace sg
