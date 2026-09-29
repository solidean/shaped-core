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

namespace sg::backend::webgpu
{
namespace
{
constexpr u32 leaf_flag = 0x80000000u;
constexpr u32 max_leaf_primitives = 4;
constexpr u32 kernel_group_size = 64;

/// WebGPU's default maxComputeWorkgroupsPerDimension, which the device is requested with.
constexpr u32 max_groups_per_dispatch = 65535;

/// The shape of a BVH2 over a primitive count, with node 0 its root.
struct tree_layout
{
    u32 node_count = 0;

    /// The leaves are the last nodes, [first_leaf, node_count).
    u32 first_leaf = 0;
    u32 leaf_count = 0;

    /// Each level's inner nodes, bottom-up, which is the order their boxes are computed in.
    struct level
    {
        u32 first = 0;
        u32 count = 0;
    };
    cc::vector<level> levels;

    /// Two units per node, their boxes zero and their `w` words set.
    cc::vector<u32> node_words;
};

/// Leaves over consecutive runs of up to 4 primitives, then each level pairing consecutive nodes, an odd one moving up unpaired.
[[nodiscard]] tree_layout lay_out_tree(u32 primitive_count)
{
    CC_ASSERT(primitive_count > 0, "a tree holds at least one primitive");
    auto const leaves = (primitive_count + max_leaf_primitives - 1) / max_leaf_primitives;

    // Nodes are made bottom-up, so the one made k-th ends up at node_count - 1 - k, and the root, made last, at 0.
    struct children
    {
        u32 left = 0;
        u32 right = 0;
    };
    auto inner = cc::vector<children>();
    auto made_levels = cc::vector<tree_layout::level>();
    auto current = cc::vector<u32>();
    for (auto leaf = u32(0); leaf < leaves; ++leaf)
        current.push_back(leaf);
    auto made = leaves;
    while (current.size() > 1)
    {
        auto next = cc::vector<u32>();
        auto const first = made;
        for (isize i = 0; i < current.size(); i += 2)
        {
            if (i + 1 < current.size())
            {
                inner.push_back({.left = current[i], .right = current[i + 1]});
                next.push_back(made++);
            }
            else
                next.push_back(current[i]);
        }
        made_levels.push_back({.first = first, .count = made - first});
        current = cc::move(next);
    }

    auto tree = tree_layout{.node_count = made, .first_leaf = made - leaves, .leaf_count = leaves};
    auto const final_index = [&](u32 made_index) { return tree.node_count - 1 - made_index; };
    tree.node_words = cc::vector<u32>::create_filled(isize(tree.node_count) * 8, u32(0));
    for (auto leaf = u32(0); leaf < leaves; ++leaf)
    {
        auto const node = isize(final_index(leaf));
        auto const first = leaf * max_leaf_primitives;
        tree.node_words[node * 8 + 3] = first;
        tree.node_words[node * 8 + 7] = cc::min(max_leaf_primitives, primitive_count - first) | leaf_flag;
    }
    for (isize k = 0; k < inner.size(); ++k)
    {
        auto const node = isize(final_index(leaves + u32(k)));
        tree.node_words[node * 8 + 3] = final_index(inner[k].left);
        tree.node_words[node * 8 + 7] = final_index(inner[k].right);
    }
    for (auto const& l : made_levels)
        tree.levels.push_back({.first = tree.node_count - (l.first + l.count), .count = l.count});
    return tree;
}

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

[[nodiscard]] u32 float_bits(float f)
{
    return cc::bit_cast<u32>(f);
}

/// The inverse of a row-major 3x4 affine transform, row-major 3x4 again.
/// A singular one, such as an instance scaled to nothing, inverts to zero, which moves every ray to the origin and away from every hit it could not have had.
void invert_affine(float const (&m)[12], float (&out)[12])
{
    auto const a = [&](int r, int c) { return double(m[r * 4 + c]); };
    double inv[3][3] = {
        {a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1), a(0, 2) * a(2, 1) - a(0, 1) * a(2, 2),
         a(0, 1) * a(1, 2) - a(0, 2) * a(1, 1)},
        {a(1, 2) * a(2, 0) - a(1, 0) * a(2, 2), a(0, 0) * a(2, 2) - a(0, 2) * a(2, 0),
         a(0, 2) * a(1, 0) - a(0, 0) * a(1, 2)},
        {a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0), a(0, 1) * a(2, 0) - a(0, 0) * a(2, 1),
         a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0)},
    };
    auto const det = a(0, 0) * inv[0][0] + a(0, 1) * inv[1][0] + a(0, 2) * inv[2][0];
    if (det == 0.0)
    {
        for (auto& v : out)
            v = 0.0f;
        return;
    }
    for (auto r = 0; r < 3; ++r)
    {
        auto translation = 0.0;
        for (auto c = 0; c < 3; ++c)
        {
            inv[r][c] /= det;
            out[r * 4 + c] = float(inv[r][c]);
        }
        for (auto c = 0; c < 3; ++c)
            translation -= inv[r][c] * a(c, 3);
        out[r * 4 + 3] = float(translation);
    }
}
} // namespace

// -- the pool, as a list records against it --

void webgpu_command_list::bring_pool_writes_forward()
{
    auto& pool = _ctx._acceleration;
    for (auto& write : _pool_writes)
    {
        if (write.generation == pool.generation())
            continue;
        auto const& current = pool.buffer();
        end_open_pass();
        wgpuCommandEncoderCopyBufferToBuffer(encoder(), write.buffer.get(), u64(write.unit) * acceleration_unit_bytes,
                                             current.get(), u64(write.unit) * acceleration_unit_bytes,
                                             u64(write.units) * acceleration_unit_bytes);
        write.generation = pool.generation();
        write.buffer = current;
    }
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
    end_open_pass();
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
                                                     cc::span<WGPUBuffer const> inputs)
{
    CC_ASSERT(inputs.size() <= 3, "a build kernel reads at most three inputs");
    auto& pool = _ctx._acceleration;
    auto const pipeline = pool.kernel(kernel);
    auto const layout = pool.kernel_layout();
    auto const pool_buffer = pool.buffer().get();

    WGPUBuffer bound_inputs[3] = {pool.unused_input(), pool.unused_input(), pool.unused_input()};
    for (isize i = 0; i < inputs.size(); ++i)
        if (inputs[i] != nullptr)
            bound_inputs[i] = inputs[i];

    constexpr auto max_items = max_groups_per_dispatch * kernel_group_size;
    open_compute_pass();
    for (auto first = u32(0); first < item_count; first += max_items)
    {
        auto const count = cc::min(item_count - first, max_items);
        args.words[0] = first;
        args.words[1] = count;
        auto const placement = _ctx._constant_pages.place(_constant_pages, cc::as_bytes(cc::span<u32 const>(args.words)));

        WGPUBindGroupEntry entries[5] = {};
        for (auto binding = 0; binding < 5; ++binding)
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
            entries[2 + i].buffer = bound_inputs[i];
        auto const desc = WGPUBindGroupDescriptor{
            .nextInChain = nullptr,
            .label = to_wgpu("sg acceleration build"),
            .layout = layout,
            .entryCount = 5,
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
    end_open_pass();
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

sg::blas_handle webgpu_command_list::raytracing_build_blas_triangles(cc::span<sg::blas_triangles const> geometries,
                                                                     sg::accel_build_flags flags,
                                                                     int /*hit_record_stride*/)
{
    check_build_flags(flags);
    CC_ASSERT(!geometries.empty(), "build_blas needs at least one geometry");

    auto triangle_count = u32(0);
    for (auto const& g : geometries)
    {
        CC_ASSERT(g.vertex_count > 0, "triangle geometry needs a positive vertex_count");
        CC_ASSERT(g.vertex_stride_in_bytes > 0 && g.vertex_stride_in_bytes % 4 == 0,
                  "webgpu reads vertices in whole words, so vertex_stride_in_bytes must be a positive multiple of 4");
        CC_ASSERT(g.vertex_offset_in_bytes >= 0 && g.vertex_offset_in_bytes % 4 == 0,
                  "webgpu reads vertices in whole words, so vertex_offset_in_bytes must be a multiple of 4");
        (void)require_build_input(g.vertices);
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

    auto const tree = lay_out_tree(triangle_count);
    auto const records = 1 + 2 * tree.node_count;
    auto const units = isize(records) + 3 * isize(triangle_count);
    auto const unit = allocate_acceleration_region(units);
    auto const first_record = unit + records;
    write_acceleration_region(
        unit, units, region_start(tree, first_record, triangle_count, u32(webgpu_blas::primitive_kind::triangles)));

    auto first_triangle = u32(0);
    for (isize geometry = 0; geometry < geometries.size(); ++geometry)
    {
        auto const& g = geometries[geometry];
        auto const count = u32(g.indices != nullptr ? g.index_count / 3 : g.vertex_count / 3);

        auto args = acceleration_kernel_args{};
        args.words[2] = first_record + 3 * first_triangle;
        args.words[3] = u32(geometry);
        args.words[4] = u32(g.vertex_offset_in_bytes / 4);
        args.words[5] = u32(g.vertex_stride_in_bytes / 4);
        args.words[6] = g.indices == nullptr ? 0u : g.index_type == sg::index_format::uint16 ? 1u : 2u;
        args.words[7] = u32(g.index_offset_in_bytes);
        args.words[8] = g.transform != nullptr ? 1u : 0u;
        args.words[9] = u32(g.transform_offset_in_bytes / 4);
        args.words[10] = g.is_opaque ? 1u : 0u;

        WGPUBuffer const inputs[3] = {
            require_build_input(g.vertices).raw(),
            g.indices != nullptr ? require_build_input(g.indices).raw() : nullptr,
            g.transform != nullptr ? require_build_input(g.transform).raw() : nullptr,
        };
        record_acceleration_kernel(acceleration_kernel::write_triangles, args, count, inputs);

        touch(g.vertices);
        if (g.indices != nullptr)
            touch(g.indices);
        if (g.transform != nullptr)
            touch(g.transform);
        first_triangle += count;
    }
    record_tree_boxes(*this, unit, tree, u32(webgpu_blas::primitive_kind::triangles), first_record);

    return std::make_shared<webgpu_blas>(_ctx, unit, u32(units), webgpu_blas::primitive_kind::triangles, flags,
                                         int(geometries.size()));
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

    auto const tree = lay_out_tree(box_count);
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
        auto args = acceleration_kernel_args{};
        args.words[2] = first_record + 2 * first_box;
        args.words[3] = u32(geometry);
        args.words[4] = u32(g.aabb_offset_in_bytes / 4);
        args.words[5] = u32(g.aabb_stride_in_bytes / 4);
        args.words[10] = g.is_opaque ? 1u : 0u;

        WGPUBuffer const inputs[1] = {require_build_input(g.aabbs).raw()};
        record_acceleration_kernel(acceleration_kernel::write_boxes, args, u32(g.aabb_count), inputs);
        touch(g.aabbs);
        first_box += u32(g.aabb_count);
    }
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
    }

    auto const instance_count = u32(instances.size());
    auto const tree = lay_out_tree(instance_count);
    auto const records = 1 + 2 * tree.node_count;
    auto const units = isize(records) + 7 * isize(instance_count);
    auto const unit = allocate_acceleration_region(units);
    auto const first_instance = unit + records;

    auto words = region_start(tree, first_instance, instance_count, kinds_present);
    words.reserve(words.size() + 28 * isize(instance_count));
    for (auto i = u32(0); i < instance_count; ++i)
    {
        auto const& inst = instances[i];
        auto const& blas = static_cast<webgpu_blas const&>(*inst.blas);

        float world_to_object[12];
        invert_affine(inst.transform, world_to_object);
        for (auto const f : world_to_object)
            words.push_back(float_bits(f));
        for (auto const f : inst.transform)
            words.push_back(float_bits(f));

        auto instance_flags = u32(0);
        if (inst.opaque_override.has_value())
            instance_flags |= inst.opaque_override.value() ? 1u : 2u;
        instance_flags |= u32(inst.cull_mode) << 2;
        words.push_back(blas.unit());
        words.push_back(inst.instance_id | (u32(inst.mask) << 24));
        words.push_back(inst.hit_group_offset | (instance_flags << 24));
        words.push_back(i);
    }
    write_acceleration_region(unit, units, words);
    record_tree_boxes(*this, unit, tree, 2, first_instance);

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
