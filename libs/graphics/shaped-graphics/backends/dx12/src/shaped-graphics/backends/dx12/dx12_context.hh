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

    /// Total descriptors in the shader-visible CBV/SRV/UAV heap binding_groups allocate their tables from.
    int descriptor_heap_capacity = 1 << 16;

    /// Share (0..1) of the descriptor heap reserved for the per-epoch-reclaimed transient ring; the
    /// rest is the persistent bump region.
    float descriptor_transient_fraction = 0.25f;
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
        _upload_async(*this)
    {
    }

    ~dx12_context() override { shutdown(); } // runs shutdown() before the base dtor asserts it

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
    [[nodiscard]] cc::result<dx12_binding_layout_handle> create_dx12_binding_layout(cc::span<sg::binding const> bindings,
                                                                                    sg::lifetime_scope scope);
    [[nodiscard]] cc::result<dx12_compute_pipeline_handle> create_dx12_compute_pipeline(sg::compiled_shader const& shader,
                                                                                        dx12_binding_layout_handle layout,
                                                                                        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<dx12_binding_group_handle> create_dx12_binding_group(dx12_binding_layout_handle layout,
                                                                                  cc::span<sg::named_view const> views,
                                                                                  sg::lifetime_scope scope);

    /// Stages a refcount-zero GPU resource for deferred deletion, attributed to the epoch it dies in
    /// (freed once that epoch retires). Called from ~dx12_buffer; safe to call from any thread.
    void schedule_deferred_deletion(dx12_expiring_resource expiring);

    // sg::context overrides — forward to the backend-typed methods above. A command list is only
    // ever a dx12 one here (backends never mix), so the downcast is sound; a CC_ASSERT'd dynamic_cast
    // guards against a foreign command list slipping in (compiled out in release).

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> create_command_list() override
    {
        return cc::result<std::unique_ptr<sg::command_list>>(create_dx12_command_list());
    }

    [[nodiscard]] cc::result<sg::raw_buffer_handle> create_raw_buffer(cc::isize size_in_bytes,
                                                                      sg::buffer_usage usage,
                                                                      sg::allocation_info const& alloc) override
    {
        return cc::result<sg::raw_buffer_handle>(create_dx12_buffer(size_in_bytes, usage, alloc));
    }

    [[nodiscard]] cc::result<sg::raw_texture_handle> create_raw_texture(sg::texture_description const& desc,
                                                                        sg::allocation_info const& alloc) override
    {
        return cc::result<sg::raw_texture_handle>(create_dx12_texture(desc, alloc));
    }

    [[nodiscard]] cc::result<sg::memory_heap_handle> create_memory_heap(cc::isize size_in_bytes) override
    {
        return cc::result<sg::memory_heap_handle>(create_dx12_memory_heap(size_in_bytes));
    }

    // Bind-path sg::context overrides — thin forwarders (unpack the description / downcast the sg layout
    // handle) to the backend-typed creates above. Bodies in dx12_bind.cc.
    [[nodiscard]] cc::result<sg::binding_layout_handle> create_binding_layout(cc::span<sg::binding const> bindings,
                                                                              sg::lifetime_scope scope) override;
    [[nodiscard]] cc::result<sg::compute_pipeline_handle> create_compute_pipeline(sg::compute_pipeline_description const& desc,
                                                                                  sg::lifetime_scope scope) override;
    [[nodiscard]] cc::result<sg::binding_group_handle> create_binding_group(sg::binding_layout_handle layout,
                                                                            cc::span<sg::named_view const> views,
                                                                            sg::lifetime_scope scope) override;

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

    // Runtime transfer-resource resizing (reached via ctx.upload / ctx.download). Each records a pending
    // change on the owning system, applied at a later safe point (see the systems + advance_epoch).
    void set_async_upload_window_bytes(cc::isize bytes) override { _upload_async.set_window_bytes(bytes); }
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

    // Dedicated COPY queue for async uploads, decoupled from the epoch/direct queue. The completion
    // fence is signaled by the copy queue when an async upload's copy has run; a later direct-queue
    // command list waits on it at submit (see submit_dx12_command_list) so it observes the write.
    ComPtr<ID3D12CommandQueue> _copy_queue;
    ComPtr<ID3D12Fence> _copy_fence;

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
};
} // namespace sg::backend::dx12

namespace sg
{
/// Creates a dx12-backed sg::context. Returns an error (never asserts) on environment failure —
/// no adapter, device creation refused, etc. Only callers that link the dx12 backend see it.
[[nodiscard]] cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config = {});
} // namespace sg
