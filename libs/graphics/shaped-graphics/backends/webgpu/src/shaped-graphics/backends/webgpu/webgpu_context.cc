// webgpu_context: construction, bring-up from a device, and shutdown.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh> // cc::min, cc::max
#include <clean-core/record/domain.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>

namespace sg::backend::webgpu
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sg.webgpu");

namespace
{
[[nodiscard]] cc::mutex<cc::vector<cc::unique_function<void()>>>& deferred_to_main()
{
    static cc::mutex<cc::vector<cc::unique_function<void()>>> queue;
    return queue;
}
} // namespace

bool impl::is_on_main_thread()
{
    return cc::current_thread_id() == cc::thread_id::main;
}

void impl::defer_to_main(cc::unique_function<void()> release)
{
    deferred_to_main().lock([&](auto& queue) { queue.push_back(cc::move(release)); });
}

void impl::drain_deferred_to_main()
{
    CC_ASSERT(is_on_main_thread(), "deferred webgpu releases are drained on the main thread");

    // Taken out under the lock and run outside it: a release can drop an sg object whose own handles queue more.
    auto pending = cc::vector<cc::unique_function<void()>>();
    deferred_to_main().lock([&](auto& queue) { pending = cc::move(queue); });
    for (auto& release : pending)
        release();
}

webgpu_context::webgpu_context(wgpu_instance instance,
                               wgpu_adapter adapter,
                               wgpu_device device,
                               std::shared_ptr<webgpu_callback_anchor> anchor,
                               webgpu_config const& config)
  : sg::context(sg::backend_kind::webgpu, sg::thread_model::main_thread, k_accepted_shader_formats),
    _anchor(cc::move(anchor)),
    _instance(cc::move(instance)),
    _adapter(cc::move(adapter)),
    _device(cc::move(device)),
    _config(config)
{
    _queue = wgpu_queue(wgpuDeviceGetQueue(_device.get()));
    _anchor->ctx = this;
}

webgpu_context::~webgpu_context()
{
    shutdown();
}

void webgpu_context::set_limits(isize uniform_offset_alignment, granted_features const& features)
{
    _uniform_offset_alignment = uniform_offset_alignment;
    _readwrite_image_formats = features.readwrite_image_formats;
    _float32_filtering = features.float32_filtering;
    _depth32_float_stencil8 = features.depth32_float_stencil8;
    _extended_image_formats = features.extended_image_formats;
    _shader_f16 = features.shader_f16;
    _subgroups = features.subgroups;
    _texture_compression_bc = features.texture_compression_bc;
    _indirect_first_instance = features.indirect_first_instance;
    _limits.max_sample_count = 4;

    _upload_ring.initialize(*this, _config.upload_ring_bytes);
    _readbacks.initialize(*this);
    _constant_pages.initialize(*this, _config.constant_page_bytes, _uniform_offset_alignment);
    _samplers.initialize(device());
    _streams.initialize(*this, _config.stream_window_bytes);
    _queries.initialize(*this, features.timestamps);
    _acceleration.initialize(*this);
}

WGPUBuffer webgpu_context::mesh_scratch_for(isize bytes)
{
    // WebGPU's default maxStorageBufferBindingSize, which the device is requested with
    constexpr auto max_bytes = isize(128) << 20;
    CC_ASSERTF(bytes <= max_bytes,
               "a polyfilled mesh draw's slots take {} bytes, past the {} a storage binding holds; draw fewer "
               "workgroups at once",
               bytes, max_bytes);
    if (_mesh_scratch.buffer && bytes <= _mesh_scratch.bytes)
        return _mesh_scratch.buffer.get();

    // Grown by half again at least, so a run of slightly larger draws grows it a few times rather than once a draw.
    auto const grown = cc::min(max_bytes, cc::max(bytes, cc::max(isize(64) << 10, _mesh_scratch.bytes * 3 / 2)));
    auto const desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg mesh polyfill slots"),
        .usage = WGPUBufferUsage_Storage,
        .size = u64(cc::max(isize(4), grown)),
        .mappedAtCreation = WGPU_FALSE,
    };
    auto buffer = wgpu_buffer(wgpuDeviceCreateBuffer(device(), &desc));
    CC_ASSERT(bool(buffer), "wgpuDeviceCreateBuffer returned no mesh polyfill buffer");

    // A list still open may have bound the old buffer, and every one of them submits before this epoch retires.
    if (_mesh_scratch.buffer)
    {
        auto expiring = webgpu_expiring_resource{};
        expiring.buffer = cc::move(_mesh_scratch.buffer);
        schedule_deferred_deletion(cc::move(expiring));
    }
    _mesh_scratch.buffer = cc::move(buffer);
    _mesh_scratch.bytes = grown;
    ++_mesh_scratch.generation;
    return _mesh_scratch.buffer.get();
}

void webgpu_context::shutdown()
{
    if (_is_shut_down)
        return;

    // Routines first: their cached state holds objects every system below owns.
    // A never-block context cannot wait for a routine's backlog, so the caller settles it before shutting down.
    routines.clear();
    release_transient_heap();
    release_cached_pipelines();
    _streams.shutdown();
    impl::drain_deferred_to_main();

    // Close the last epoch so its transient resources expire and their finalizers run below.
    if (_open_command_lists == 0 && _device)
        advance_epoch();
    retire_completed_epochs();
    // what was read back already is decoded; a never-block context waits for nothing still in flight
    release_debug_windows();

    // No callback reaches this context from here on, whatever the queue still reports.
    _anchor->ctx = nullptr;
    _anchor->on_error = {};
    stop_completion_signals();

    // What never retired is released unretired: every WebGPU object is reference counted, so work still queued keeps
    // what it names alive, and only the finalizers are owed.
    auto finalizers = cc::vector<cc::unique_function<void()>>();
    while (!_epochs.in_flight.empty())
    {
        auto data = _epochs.in_flight.pop_front();
        for (auto& res : data.expiring)
            for (auto& f : res.finalizers)
                finalizers.push_back(cc::move(f));
    }
    for (auto& res : _epochs.staged)
        for (auto& f : res.finalizers)
            finalizers.push_back(cc::move(f));
    _epochs.staged = {};

    _queries.shutdown();
    _acceleration.shutdown();
    _mesh_scratch = {};
    _constant_pages.shutdown();
    _readbacks.shutdown();
    _upload_ring.shutdown();
    _samplers.shutdown();

    _is_shut_down = true;
    for (auto& f : finalizers)
        f();

    _queue = {};
    _device = {};
    _adapter = {};
    _instance = {};
}
} // namespace sg::backend::webgpu
