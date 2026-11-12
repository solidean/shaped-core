// webgpu ray tracing: acceleration-structure builds into the ray-query polyfill's pool, and the refused pipeline.
//
// A build lays its tree out on the CPU, since the topology depends on the counts alone.
// It uploads that together with the header, and for a TLAS the instances, then records compute work for everything that reads the inputs.
// The layout is libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <shaped-graphics/backends/webgpu/webgpu_acceleration.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>

namespace sg::backend::webgpu
{
namespace
{
constexpr u32 kernel_group_size = 64;

/// WebGPU's default maxComputeWorkgroupsPerDimension, which the device is requested with.
constexpr u32 max_groups_per_dispatch = 65535;

/// The largest minStorageBufferOffsetAlignment WebGPU allows, so a binding offset rounded down to it suits every device.
constexpr isize storage_offset_alignment = 256;

using tree_layout = sg::impl::acceleration_tree;

void check_build_flags(sg::accel_build_flags flags)
{
    CC_ASSERT(!flags.has_all(sg::accel_build_flag::fast_trace | sg::accel_build_flag::fast_build),
              "fast_trace and fast_build are mutually exclusive");
}

/// Downcasts and validates a build-input buffer, asserting the common misuses.
[[nodiscard]] webgpu_buffer const& require_build_input(sg::raw_buffer_handle const& buffer)
{
    CC_ASSERT(buffer != nullptr, "acceleration-structure build input buffer is null");
    auto const* b = dynamic_cast<webgpu_buffer const*>(buffer.get());
    CC_ASSERT(b != nullptr, "build input buffer is not a webgpu buffer");
    CC_ASSERT(!b->is_expired(), "build input buffer is a transient buffer used past its epoch (expired)");
    CC_ASSERT(b->usage().has(sg::buffer_usage::accel_structure_build_input),
              "acceleration-structure build input buffer must have buffer_usage::accel_structure_build_input");
    return *b;
}

/// A build input bound as the bytes [first, end) of its buffer rather than whole, so a range in a buffer larger than
/// maxStorageBufferBindingSize still binds.
/// The start rounds down to the storage offset alignment, and `lead_in_bytes` is how far: a kernel's offsets are
/// relative to the bound start, so each one is this lead.
struct input_range
{
    webgpu_command_list::acceleration_kernel_input input;
    isize lead_in_bytes = 0;
};

[[nodiscard]] input_range bind_input_range(webgpu_buffer const& buffer, isize first, isize end)
{
    CC_ASSERT(0 <= first && first < end, "a build input range holds at least one byte");
    // A webgpu buffer is allocated in whole words, so a range may end at the word its last byte sits in.
    CC_ASSERT(end <= cc::align_up(buffer.size_in_bytes(), 4), "a build input reads past the end of its buffer");
    auto const start = cc::align_down(first, storage_offset_alignment);
    return {
        .input = {.buffer = buffer.raw(), .offset = u64(start), .size = u64(cc::align_up(end, 4) - start)},
        .lead_in_bytes = first - start,
    };
}

} // namespace

// -- the pool, as a list records against it --

void webgpu_command_list::bring_pool_writes_forward()
{
    auto& pool = _ctx._acceleration;
    if (_pool_writes_generation == pool.generation())
        return;
    for (auto& write : _pool_writes)
    {
        if (write.generation == pool.generation())
            continue;
        auto const& current = pool.buffer();
        end_open_pass("a copy");
        wgpuCommandEncoderCopyBufferToBuffer(encoder(), write.buffer.get(), u64(write.unit) * acceleration_unit_bytes,
                                             current.get(), u64(write.unit) * acceleration_unit_bytes,
                                             u64(write.units) * acceleration_unit_bytes);
        write.generation = pool.generation();
        write.buffer = current;
    }
    _pool_writes_generation = pool.generation();
}

u32 webgpu_command_list::allocate_acceleration_region(isize units)
{
    CC_ASSERT(!_in_rendering_scope, "an acceleration structure must not be built inside a rendering scope; close the "
                                    "scope first");
    auto const unit = _ctx._acceleration.allocate(units);
    bring_pool_writes_forward();
    return unit;
}

void webgpu_command_list::write_acceleration_region(u32 unit, isize units, cc::span<u32 const> cpu_part)
{
    auto const& pool = _ctx._acceleration.buffer();
    auto const bytes = cc::as_bytes(cpu_part);
    auto const span = stage_upload(bytes, bytes.size());
    end_open_pass("a copy");
    wgpuCommandEncoderCopyBufferToBuffer(encoder(), span.buffer, u64(span.offset), pool.get(),
                                         u64(unit) * acceleration_unit_bytes, u64(bytes.size()));
    if (span.overflow)
        _keep_alive.push_back(std::make_shared<wgpu_buffer>(span.overflow));
    _pool_writes.push_back(
        {.generation = _ctx._acceleration.generation(), .buffer = pool, .unit = unit, .units = u32(units)});
}

void webgpu_command_list::record_acceleration_kernel(acceleration_kernel kernel,
                                                     acceleration_kernel_args args,
                                                     u32 item_count,
                                                     cc::span<acceleration_kernel_input const> inputs)
{
    CC_ASSERT(inputs.size() <= 3, "a build kernel reads at most three inputs");
    auto& pool = _ctx._acceleration;
    auto const pipeline = pool.kernel(kernel);
    auto const layout = pool.kernel_layout();
    auto const pool_buffer = pool.buffer().get();
    auto const scratch_buffer = pool.sort_scratch().get();

    auto const unused = acceleration_kernel_input{.buffer = pool.unused_input(), .offset = 0, .size = WGPU_WHOLE_SIZE};
    acceleration_kernel_input bound_inputs[3] = {unused, unused, unused};
    for (isize i = 0; i < inputs.size(); ++i)
        if (inputs[i].buffer != nullptr)
            bound_inputs[i] = inputs[i];

    constexpr auto max_items = max_groups_per_dispatch * kernel_group_size;
    open_compute_pass();
    for (auto first = u32(0); first < item_count; first += max_items)
    {
        auto const count = cc::min(item_count - first, max_items);
        args.words[0] = first;
        args.words[1] = count;
        auto const placement = _ctx._constant_pages.place(_constant_pages, cc::as_bytes(cc::span<u32 const>(args.words)));

        WGPUBindGroupEntry entries[6] = {};
        for (auto binding = 0; binding < 6; ++binding)
        {
            entries[binding].binding = u32(binding);
            entries[binding].offset = 0;
            entries[binding].size = WGPU_WHOLE_SIZE;
        }
        entries[0].buffer = placement.page->buffer.get();
        entries[0].offset = placement.offset;
        entries[0].size = sizeof(args.words);
        entries[1].buffer = pool_buffer;
        for (auto i = 0; i < 3; ++i)
        {
            entries[2 + i].buffer = bound_inputs[i].buffer;
            entries[2 + i].offset = bound_inputs[i].offset;
            entries[2 + i].size = bound_inputs[i].size;
        }
        entries[5].buffer = scratch_buffer;
        auto const desc = WGPUBindGroupDescriptor{
            .nextInChain = nullptr,
            .label = to_wgpu("sg acceleration build"),
            .layout = layout,
            .entryCount = 6,
            .entries = entries,
        };
        auto const group = wgpu_bind_group(wgpuDeviceCreateBindGroup(_ctx.device(), &desc));

        wgpuComputePassEncoderSetPipeline(compute_pass(), pipeline);
        wgpuComputePassEncoderSetBindGroup(compute_pass(), 0, group.get(), 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(compute_pass(), (count + kernel_group_size - 1) / kernel_group_size, 1,
                                                 1);
    }

    // The caller's pipeline and groups are no longer what the pass has bound.
    _compute.needs_full_apply = true;
}

sg::bytes_future webgpu_command_list::download_acceleration_pool()
{
    bring_pool_writes_forward();
    auto const& pool = _ctx._acceleration.buffer();
    auto const bytes = _ctx._acceleration.capacity_units() * acceleration_unit_bytes;
    auto readback = _ctx._readbacks.acquire(bytes);
    end_open_pass("a copy");
    wgpuCommandEncoderCopyBufferToBuffer(encoder(), pool.get(), 0, readback.staging.get(), 0, u64(bytes));

    auto destination = cc::pinned_data<byte>::create_uninitialized(bytes);
    auto const dst_span = destination.span();
    return record_readback(cc::move(readback), cc::move(destination),
                           [dst_span](cc::span<byte const> mapped)
                           {
                               cc::memcpy(dst_span.data(), mapped.data(), size_t(dst_span.size()));
                               return true;
                           });
}

// -- builds --

namespace
{
/// Records the leaf boxes, then every level's, over a region laid out as `tree`.
void record_tree_boxes(webgpu_command_list& cmd, u32 unit, tree_layout const& tree, u32 leaf_kind, u32 first_primitive_unit)
{
    auto leaves = webgpu_command_list::acceleration_kernel_args{};
    leaves.words[2] = unit;
    leaves.words[3] = leaf_kind;
    leaves.words[4] = tree.first_leaf;
    leaves.words[5] = first_primitive_unit;
    cmd.record_acceleration_kernel(acceleration_kernel::write_leaves, leaves, tree.leaf_count);

    for (auto const& level : tree.levels)
    {
        auto args = webgpu_command_list::acceleration_kernel_args{};
        args.words[2] = unit;
        args.words[4] = level.first;
        cmd.record_acceleration_kernel(acceleration_kernel::write_levels, args, level.count);
    }
}

/// Below this many records a sort buys nothing measurable, and the record order stays the caller's.
constexpr u32 spatial_sort_min_records = 16;

/// The header and the nodes, the start of every region.
[[nodiscard]] cc::vector<u32> region_start(tree_layout const& tree, u32 first_record_unit, u32 record_count, u32 kind_word)
{
    auto words = cc::vector<u32>();
    words.reserve(4 + tree.node_words.size());
    words.push_back(tree.node_count);
    words.push_back(first_record_unit);
    words.push_back(record_count);
    words.push_back(kind_word);
    words.push_back_range(tree.node_words);
    return words;
}
} // namespace

void webgpu_command_list::record_spatial_sort(u32 record_kind, u32 first_record_unit, u32 record_units, u32 record_count)
{
    if (record_count < spatial_sort_min_records)
        return;

    // A block per invocation of the count and scatter passes: enough blocks to occupy a GPU, few enough that one
    // invocation per digit can scan its row of them.
    auto const blocks = cc::clamp((record_count + 255) / 256, u32(1), u32(4096));
    auto const per_block = (record_count + blocks - 1) / blocks;

    // The scratch, in words: the centers' bounds, the row bases, the counts, two key and value arrays, the gathered records.
    auto const row_bases = u32(8);
    auto const counts = row_bases + 256;
    auto const keys_a = counts + 256 * blocks;
    auto const values_a = keys_a + record_count;
    auto const keys_b = values_a + record_count;
    auto const values_b = keys_b + record_count;
    auto const gathered = values_b + record_count;
    auto const words = isize(gathered) + 4 * isize(record_units) * isize(record_count);
    if (words * 4 > max_sort_scratch_bytes)
        return; // larger than one storage binding holds: kept in the caller's order, which traces correctly, only slower

    // A growth replaces the buffer an earlier build of this list may have bound.
    auto& pool = _ctx._acceleration;
    _keep_alive.push_back(std::make_shared<wgpu_buffer>(pool.sort_scratch()));
    (void)pool.sort_scratch(words);

    auto const clear = [&](u32 first, u32 count, u32 value)
    {
        auto args = acceleration_kernel_args{};
        args.words[6] = first;
        args.words[7] = value;
        record_acceleration_kernel(acceleration_kernel::sort_clear, args, count);
    };

    // What every kernel reading the records needs: what they are, and where the first one is.
    auto records = acceleration_kernel_args{};
    records.words[3] = record_kind;
    records.words[5] = first_record_unit;

    clear(0, 3, 0xffffffffu);
    clear(3, 3, 0u);
    record_acceleration_kernel(acceleration_kernel::sort_bounds, records, record_count);
    auto keys = records;
    keys.words[8] = keys_a;
    keys.words[9] = values_a;
    record_acceleration_kernel(acceleration_kernel::sort_keys, keys, record_count);

    // Four passes of 8 bits cover the 30 bits of a code, and an even count leaves the result where it started.
    for (auto pass = u32(0); pass < 4; ++pass)
    {
        auto const from_a = pass % 2 == 0;
        auto args = acceleration_kernel_args{};
        args.words[6] = from_a ? keys_b : keys_a;
        args.words[7] = from_a ? values_b : values_a;
        args.words[8] = from_a ? keys_a : keys_b;
        args.words[9] = from_a ? values_a : values_b;
        args.words[10] = counts;
        args.words[11] = row_bases;
        args.words[12] = record_count;
        args.words[13] = blocks;
        args.words[14] = 8 * pass;
        args.words[15] = per_block;
        clear(counts, 256 * blocks, 0u);
        record_acceleration_kernel(acceleration_kernel::sort_count, args, blocks);
        record_acceleration_kernel(acceleration_kernel::sort_scan_rows, args, 256);
        record_acceleration_kernel(acceleration_kernel::sort_scan_bases, args, 1);
        record_acceleration_kernel(acceleration_kernel::sort_scatter, args, blocks);
    }

    auto gather = acceleration_kernel_args{};
    gather.words[4] = record_units;
    gather.words[5] = first_record_unit;
    gather.words[6] = gathered;
    gather.words[9] = values_a;
    record_acceleration_kernel(acceleration_kernel::sort_gather, gather, record_count);

    end_open_pass("a copy");
    wgpuCommandEncoderCopyBufferToBuffer(encoder(), pool.sort_scratch().get(), u64(gathered) * 4, pool.buffer().get(),
                                         u64(first_record_unit) * acceleration_unit_bytes,
                                         u64(record_count) * u64(record_units) * acceleration_unit_bytes);
}

sg::blas_handle webgpu_command_list::raytracing_build_blas_triangles(cc::span<sg::blas_triangles const> geometries,
                                                                     sg::accel_build_flags flags,
                                                                     int /*hit_record_stride*/)
{
    check_build_flags(flags);
    CC_ASSERT(!geometries.empty(), "build_blas needs at least one geometry");

    auto triangle_count = u32(0);
    auto is_moving = false;
    for (auto const& g : geometries)
    {
        CC_ASSERT(g.vertex_count > 0, "triangle geometry needs a positive vertex_count");
        CC_ASSERT(g.vertex_stride_in_bytes > 0 && g.vertex_stride_in_bytes % 4 == 0,
                  "webgpu reads vertices in whole words, so vertex_stride_in_bytes must be a positive multiple of 4");
        CC_ASSERT(g.vertex_offset_in_bytes >= 0 && g.vertex_offset_in_bytes % 4 == 0,
                  "webgpu reads vertices in whole words, so vertex_offset_in_bytes must be a multiple of 4");
        (void)require_build_input(g.vertices);
        for (auto const& pose : g.later_poses)
        {
            CC_ASSERT(pose.offset_in_bytes >= 0 && pose.offset_in_bytes % 4 == 0,
                      "webgpu reads vertices in whole words, so a later pose's offset_in_bytes must be a multiple of "
                      "4");
            (void)require_build_input(pose.vertices);
            is_moving = true;
        }
        if (g.indices != nullptr)
        {
            CC_ASSERT(g.index_count > 0 && g.index_count % 3 == 0, "indexed triangles need index_count > 0 and a "
                                                                   "multiple of 3");
            CC_ASSERT(g.index_offset_in_bytes >= 0
                          && g.index_offset_in_bytes % (g.index_type == sg::index_format::uint16 ? 2 : 4) == 0,
                      "index_offset_in_bytes must be a multiple of the index width");
            (void)require_build_input(g.indices);
            triangle_count += u32(g.index_count / 3);
        }
        else
        {
            CC_ASSERT(g.vertex_count % 3 == 0, "non-indexed triangles need vertex_count to be a multiple of 3");
            triangle_count += u32(g.vertex_count / 3);
        }
        if (g.transform != nullptr)
        {
            CC_ASSERT(g.transform_offset_in_bytes >= 0 && g.transform_offset_in_bytes % 4 == 0,
                      "transform_offset_in_bytes must be a multiple of 4");
            (void)require_build_input(g.transform);
        }
    }

    auto const tree = sg::impl::lay_out_acceleration_tree(triangle_count);
    auto const records = 1 + 2 * tree.node_count;
    // a BLAS with a moving geometry gives each triangle its pose at time 1 too, in units 3 to 5
    auto const stride = is_moving ? u32(6) : u32(3);
    auto const units = isize(records) + isize(stride) * isize(triangle_count);
    auto const unit = allocate_acceleration_region(units);
    auto const first_record = unit + records;
    auto const kind_word = u32(webgpu_blas::primitive_kind::triangles) | (is_moving ? 4u : 0u);
    write_acceleration_region(unit, units, region_start(tree, first_record, triangle_count, kind_word));

    auto first_triangle = u32(0);
    for (isize geometry = 0; geometry < geometries.size(); ++geometry)
    {
        auto const& g = geometries[geometry];
        auto const count = u32(g.indices != nullptr ? g.index_count / 3 : g.vertex_count / 3);

        // Every index is below vertex_count, so no vertex past the geometry's last one is read.
        auto const vertices_at = [&](sg::raw_buffer_handle const& buffer, isize offset)
        {
            return bind_input_range(require_build_input(buffer), offset,
                                    offset + (g.vertex_count - 1) * g.vertex_stride_in_bytes + 3 * isize(sizeof(float)));
        };
        auto const vertices = vertices_at(g.vertices, g.vertex_offset_in_bytes);
        auto indices = input_range{};
        if (g.indices != nullptr)
        {
            auto const index_bytes = g.index_type == sg::index_format::uint16 ? isize(2) : isize(4);
            indices = bind_input_range(require_build_input(g.indices), g.index_offset_in_bytes,
                                       g.index_offset_in_bytes + g.index_count * index_bytes);
        }
        auto transform = input_range{};
        if (g.transform != nullptr)
            transform = bind_input_range(require_build_input(g.transform), g.transform_offset_in_bytes,
                                         g.transform_offset_in_bytes + 12 * isize(sizeof(float)));

        auto args = acceleration_kernel_args{};
        args.words[2] = first_record + stride * first_triangle;
        args.words[3] = u32(geometry);
        args.words[4] = u32(vertices.lead_in_bytes / 4);
        args.words[5] = u32(g.vertex_stride_in_bytes / 4);
        args.words[6] = g.indices == nullptr ? 0u : g.index_type == sg::index_format::uint16 ? 1u : 2u;
        args.words[7] = u32(indices.lead_in_bytes);
        args.words[8] = g.transform != nullptr ? 1u : 0u;
        args.words[9] = u32(transform.lead_in_bytes / 4);
        args.words[10] = g.is_opaque ? 1u : 0u;
        args.words[11] = stride;

        acceleration_kernel_input const inputs[3] = {vertices.input, indices.input, transform.input};
        record_acceleration_kernel(acceleration_kernel::write_triangles, args, count, inputs);

        // a still geometry of a moving BLAS ends where it starts
        if (is_moving)
        {
            auto const end = g.later_poses.empty()
                               ? vertices
                               : vertices_at(g.later_poses[0].vertices, g.later_poses[0].offset_in_bytes);
            args.words[4] = u32(end.lead_in_bytes / 4);
            acceleration_kernel_input const end_inputs[3] = {end.input, indices.input, transform.input};
            record_acceleration_kernel(acceleration_kernel::write_triangle_ends, args, count, end_inputs);
        }

        touch(g.vertices);
        for (auto const& pose : g.later_poses)
            touch(pose.vertices);
        if (g.indices != nullptr)
            touch(g.indices);
        if (g.transform != nullptr)
            touch(g.transform);
        first_triangle += count;
    }
    // A moving BLAS's records hold both poses, which the spatial sort's fixed stride does not.
    if (!flags.has(sg::accel_build_flag::fast_build) && !is_moving)
        record_spatial_sort(u32(webgpu_blas::primitive_kind::triangles), first_record, 3, triangle_count);
    // leaf kind 4 is triangles that move, whose box holds both of their poses
    record_tree_boxes(*this, unit, tree, is_moving ? 4 : u32(webgpu_blas::primitive_kind::triangles), first_record);

    return std::make_shared<webgpu_blas>(_ctx, unit, u32(units), webgpu_blas::primitive_kind::triangles, flags,
                                         int(geometries.size()));
}

sg::micromap_handle webgpu_command_list::raytracing_build_micromap(sg::micromap_triangles const& /*triangles*/)
{
    CC_UNREACHABLE("the ray-query polyfill has no opacity micromaps, and webgpu reports no "
                   "sg::feature::opacity_micromap");
}

sg::blas_handle webgpu_command_list::raytracing_build_blas_aabbs(cc::span<sg::blas_aabbs const> geometries,
                                                                 sg::accel_build_flags flags,
                                                                 int /*hit_record_stride*/)
{
    check_build_flags(flags);
    CC_ASSERT(!geometries.empty(), "build_blas needs at least one geometry");

    auto box_count = u32(0);
    for (auto const& g : geometries)
    {
        CC_ASSERT(g.aabb_count > 0, "procedural geometry needs a positive aabb_count");
        CC_ASSERT(g.aabb_stride_in_bytes > 0 && g.aabb_stride_in_bytes % 8 == 0, "aabb_stride_in_bytes must be "
                                                                                 "positive and a multiple of 8");
        CC_ASSERT(g.aabb_offset_in_bytes >= 0 && g.aabb_offset_in_bytes % 4 == 0,
                  "webgpu reads boxes in whole words, so aabb_offset_in_bytes must be a multiple of 4");
        (void)require_build_input(g.aabbs);
        box_count += u32(g.aabb_count);
    }

    auto const tree = sg::impl::lay_out_acceleration_tree(box_count);
    auto const records = 1 + 2 * tree.node_count;
    auto const units = isize(records) + 2 * isize(box_count);
    auto const unit = allocate_acceleration_region(units);
    auto const first_record = unit + records;
    write_acceleration_region(unit, units,
                              region_start(tree, first_record, box_count, u32(webgpu_blas::primitive_kind::boxes)));

    auto first_box = u32(0);
    for (isize geometry = 0; geometry < geometries.size(); ++geometry)
    {
        auto const& g = geometries[geometry];
        auto const boxes = bind_input_range(
            require_build_input(g.aabbs), g.aabb_offset_in_bytes,
            g.aabb_offset_in_bytes + (g.aabb_count - 1) * g.aabb_stride_in_bytes + 6 * isize(sizeof(float)));

        auto args = acceleration_kernel_args{};
        args.words[2] = first_record + 2 * first_box;
        args.words[3] = u32(geometry);
        args.words[4] = u32(boxes.lead_in_bytes / 4);
        args.words[5] = u32(g.aabb_stride_in_bytes / 4);
        args.words[10] = g.is_opaque ? 1u : 0u;

        acceleration_kernel_input const inputs[1] = {boxes.input};
        record_acceleration_kernel(acceleration_kernel::write_boxes, args, u32(g.aabb_count), inputs);
        touch(g.aabbs);
        first_box += u32(g.aabb_count);
    }
    if (!flags.has(sg::accel_build_flag::fast_build))
        record_spatial_sort(u32(webgpu_blas::primitive_kind::boxes), first_record, 2, box_count);
    record_tree_boxes(*this, unit, tree, u32(webgpu_blas::primitive_kind::boxes), first_record);

    return std::make_shared<webgpu_blas>(_ctx, unit, u32(units), webgpu_blas::primitive_kind::boxes, flags,
                                         int(geometries.size()));
}

sg::tlas_handle webgpu_command_list::raytracing_build_tlas(cc::span<sg::tlas_instance const> instances,
                                                           sg::accel_build_flags flags)
{
    check_build_flags(flags);
    CC_ASSERT(!instances.empty(), "build_tlas needs at least one instance");

    auto referenced_blases = cc::vector<sg::blas_handle>();
    auto kinds_present = u32(0);
    auto is_moving = false;
    for (auto const& inst : instances)
    {
        CC_ASSERT(inst.blas != nullptr, "tlas_instance.blas is null");
        CC_ASSERT(!inst.blas->is_expired(), "tlas_instance.blas is expired");
        CC_ASSERT(inst.instance_id < (1u << 24), "tlas_instance.instance_id must fit in 24 bits");
        CC_ASSERT(inst.hit_group_offset < (1u << 24), "tlas_instance.hit_group_offset must fit in 24 bits");
        auto const* blas = dynamic_cast<webgpu_blas const*>(inst.blas.get());
        CC_ASSERT(blas != nullptr, "tlas_instance.blas is not a webgpu blas");
        kinds_present |= 1u << u32(blas->kind());
        referenced_blases.push_back(inst.blas);
        is_moving = is_moving || inst.transform_at_end.has_value();
    }

    auto const instance_count = u32(instances.size());
    auto const tree = sg::impl::lay_out_acceleration_tree(instance_count);
    auto const records = 1 + 2 * tree.node_count;
    // a TLAS with a moving instance gives each instance its transform at time 1 too, in units 7 to 9
    auto const stride = is_moving ? isize(10) : isize(7);
    auto const units = isize(records) + stride * isize(instance_count);
    auto const unit = allocate_acceleration_region(units);
    auto const first_instance = unit + records;

    auto words = region_start(tree, first_instance, instance_count, kinds_present | (is_moving ? 4u : 0u));
    words.reserve(words.size() + 4 * stride * isize(instance_count));
    for (auto i = u32(0); i < instance_count; ++i)
    {
        sg::impl::append_instance_words(words, instances[i], static_cast<webgpu_blas const&>(*instances[i].blas).unit(),
                                        i, is_moving);
    }
    write_acceleration_region(unit, units, words);
    if (!flags.has(sg::accel_build_flag::fast_build) && !is_moving)
        record_spatial_sort(2, first_instance, 7, instance_count);
    // leaf kind 3 is instances that move, whose box holds both of their transforms' boxes
    record_tree_boxes(*this, unit, tree, is_moving ? 3 : 2, first_instance);

    return std::make_shared<webgpu_tlas>(_ctx, unit, u32(units), flags, int(instance_count), cc::move(referenced_blases));
}

// -- the ray-tracing pipeline: refused --

void webgpu_command_list::raytracing_bind_pipeline(sg::raytracing_pipeline const&)
{
    CC_UNREACHABLE("webgpu has no ray-tracing pipeline, only ray queries; check "
                   "ctx.supports(sg::feature::raytracing_pipeline)");
}

void webgpu_command_list::raytracing_bind_group(int, sg::binding_group const&)
{
    CC_UNREACHABLE("webgpu has no ray-tracing pipeline, only ray queries; check "
                   "ctx.supports(sg::feature::raytracing_pipeline)");
}

void webgpu_command_list::raytracing_dispatch_rays(sg::raytracing_shader_table const&, sg::raygen_index, int, int, int)
{
    CC_UNREACHABLE("webgpu has no ray-tracing pipeline, only ray queries; check "
                   "ctx.supports(sg::feature::raytracing_pipeline)");
}
} // namespace sg::backend::webgpu
