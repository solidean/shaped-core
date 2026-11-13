#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <shaped-viewer/resources/impl/mip_layout.hh>
#include <shaped-viewer/resources/resource_managers.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <sv_shaders.hh>

namespace sv
{
mesh_manager mesh_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto manager = mesh_manager(ctx);
    manager.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return manager;
}

namespace
{
/// `accel_structure_build_input` where `ctx` builds acceleration structures at all, and nothing elsewhere.
/// Vulkan refuses the usage on a device without the extension.
[[nodiscard]] sg::buffer_usages build_input_usage(sg::context const& ctx)
{
    auto const builds = ctx.supports(sg::feature::ray_query) || ctx.supports(sg::feature::raytracing_pipeline);
    return builds ? sg::buffer_usages(sg::buffer_usage::accel_structure_build_input) : sg::buffer_usages();
}

// Build input for the BLAS a trace asks for; readonly_buffer so a hit, or a raster vertex stage, reads the geometry back.
[[nodiscard]] sg::buffer_usages geometry_usage(sg::context const& ctx)
{
    return build_input_usage(ctx) | sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst;
}


/// The area of the triangle over `a`, `b` and `c`.
[[nodiscard]] f64 triangle_area(tg::pos3f a, tg::pos3f b, tg::pos3f c)
{
    auto const e1 = b - a;
    auto const e2 = c - a;
    auto const x = f64(e1[1]) * f64(e2[2]) - f64(e1[2]) * f64(e2[1]);
    auto const y = f64(e1[2]) * f64(e2[0]) - f64(e1[0]) * f64(e2[2]);
    auto const z = f64(e1[0]) * f64(e2[1]) - f64(e1[1]) * f64(e2[0]);
    return 0.5 * tg::sqrt(x * x + y * y + z * z);
}
} // namespace

namespace impl
{
// What the streaming actor orders our transfers by; higher runs first.
//
// Attributes outrank the geometry that indexes them, and that ordering is a correctness argument rather than a
// preference: a mesh becomes drawable the moment its BLAS is built, and if its uv set were still in flight then it
// would draw its real triangles against zeroed attribute bytes for a frame or two.
// Geometry outranks textures for the reason the design gives: a gray model beats a floating albedo map.
constexpr i32 attribute_stream_priority = 20;
constexpr i32 geometry_stream_priority = 10;
constexpr i32 texture_stream_priority = 0;
} // namespace impl

namespace
{
// A quadric record and its box, in the vertex pool's 12-byte elements.
constexpr isize primitive_elements = 8;
constexpr isize box_elements = 2;

// Room for a small scene before the first growth: 768 KB of positions and as many bytes of indices.
constexpr isize initial_pool_vertices = isize(1) << 16;
constexpr isize initial_pool_indices = isize(3) << 16;

// A pool is also the source of the copy that grows it.
[[nodiscard]] sg::buffer_usages pool_usage(sg::context const& ctx)
{
    return geometry_usage(ctx) | sg::buffer_usage::copy_src;
}

// The index pool is also what an indexed raster draw binds.
[[nodiscard]] sg::buffer_usages index_pool_usage(sg::context const& ctx)
{
    return pool_usage(ctx) | sg::buffer_usage::index_buffer;
}

// A mesh's payload streams into buffers of its own, and is copied into the pools once it landed.
constexpr auto staging_usage = sg::buffer_usage::copy_dst | sg::buffer_usage::copy_src;

/// At least twice `capacity`, doubled further until it holds `min_capacity`.
[[nodiscard]] isize grown_capacity(isize capacity, isize min_capacity)
{
    auto grown = capacity > 0 ? 2 * capacity : isize(1);
    while (grown < min_capacity)
        grown *= 2;
    return grown;
}
} // namespace

mesh_id mesh_manager::acquire(triangle_data const& mesh)
{
    if (auto const id = find_by_hash(mesh.hash); id.has_value())
        return id.value();

    auto const positions = mesh.positions.span();
    CC_ASSERT(!positions.empty() && positions.size() % 3 == 0, "mesh_manager::acquire expects a triangle list (vertex "
                                                               "count a non-zero multiple of 3)");

    // Each corner is its own vertex, so the indices are the numbering its per-vertex attributes already use.
    auto sequential = cc::vector<u32>();
    sequential.reserve(positions.size());
    for (auto i = isize(0); i < positions.size(); ++i)
        sequential.push_back(u32(i));

    // Summed while the positions are at hand, since the payload is the stream's from here on.
    auto area = 0.0;
    for (auto i = isize(0); i + 2 < positions.size(); i += 3)
        area += triangle_area(positions[i], positions[i + 1], positions[i + 2]);

    return _insert(mesh.hash, mesh.positions, cc::make_pinned_data(cc::move(sequential)), f32(area), mesh.bounds);
}

mesh_id mesh_manager::acquire(indexed_triangle_data const& mesh)
{
    if (auto const id = find_by_hash(mesh.hash); id.has_value())
        return id.value();

    auto const positions = mesh.positions.span();
    auto const indices = mesh.indices.span();
    CC_ASSERT(!indices.empty() && indices.size() % 3 == 0, "mesh_manager::acquire expects an index count that is a "
                                                           "non-zero multiple of 3");
    CC_ASSERT(!positions.empty(), "mesh_manager::acquire needs at least one vertex");

    // An index past the positions adds nothing here; the trace would read garbage there anyway.
    auto area = 0.0;
    auto const vertex_count = u32(positions.size());
    for (auto i = isize(0); i + 2 < indices.size(); i += 3)
        if (indices[i] < vertex_count && indices[i + 1] < vertex_count && indices[i + 2] < vertex_count)
            area += triangle_area(positions[indices[i]], positions[indices[i + 1]], positions[indices[i + 2]]);

    return _insert(mesh.hash, mesh.positions, mesh.indices, f32(area), mesh.bounds);
}

mesh_id mesh_manager::_insert(cc::hash128 hash,
                              cc::pinned_data<tg::pos3f const> positions,
                              cc::pinned_data<u32 const> indices,
                              f32 surface_area,
                              cc::optional<tg::aabb3f> bounds)
{
    _ensure_pools();
    auto const geometry = _allocate(positions.size(), indices.size() / 3);

    // The BLAS is not charged yet, since it is not built: `acquire_blas` adds its size once it is.
    auto const size_in_bytes = positions.size() * isize(sizeof(tg::pos3f)) + indices.size() * isize(sizeof(u32));

    // Streamed into staging rather than into the pools: a list touching a buffer a stream still fills waits for it,
    // and every frame touches the pools.
    auto pending = pending_mesh{
        .staged_vertices = _ctx.persistent.create_buffer<tg::pos3f>(positions.size(), staging_usage),
        .staged_indices = _ctx.persistent.create_buffer<u32>(indices.size(), staging_usage),
        .bytes = size_in_bytes,
    };
    pending.vertices = _ctx.stream.data_to_buffer(pending.staged_vertices, cc::move(positions));
    pending.indices = _ctx.stream.data_to_buffer(pending.staged_indices, cc::move(indices));
    pending.vertices.set_priority(impl::geometry_stream_priority);
    pending.indices.set_priority(impl::geometry_stream_priority);

    auto const id = insert(
        hash, {.state = residency::pending, .geometry = geometry, .surface_area = surface_area, .bounds = bounds},
        size_in_bytes);
    _settling[id] = cc::move(pending);
    return id;
}

void mesh_manager::_ensure_pools()
{
    if (_vertex_pool.raw() != nullptr)
        return;

    _vertex_pool = _ctx.persistent.create_buffer<tg::pos3f>(initial_pool_vertices, pool_usage(_ctx));
    _index_pool = _ctx.persistent.create_buffer<u32>(initial_pool_indices, index_pool_usage(_ctx));
    _vertex_ranges.grow(initial_pool_vertices);
    _index_ranges.grow(initial_pool_indices);

    // The unit cube as a raw triangle list: 12 triangles over the corners of [0,1]^3, wound outward.
    // Spelled out rather than generated, because it is read once and a loop over face tables would be longer.
    constexpr tg::pos3f c[8] = {tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(1, 1, 0), tg::pos3f(0, 1, 0),
                                tg::pos3f(0, 0, 1), tg::pos3f(1, 0, 1), tg::pos3f(1, 1, 1), tg::pos3f(0, 1, 1)};
    constexpr int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};

    auto positions = cc::vector<tg::pos3f>();
    auto indices = cc::vector<u32>();
    positions.reserve(36);
    indices.reserve(36);
    for (auto const& q : quads)
        for (auto const i : {0, 1, 2, 0, 2, 3})
        {
            indices.push_back(u32(positions.size()));
            positions.push_back(c[q[i]]);
        }

    _placeholder = _allocate(positions.size(), indices.size() / 3);

    // Uploaded rather than streamed: every pending mesh names these ranges, so nothing may wait on them.
    auto up = _ctx.create_command_list();
    up->upload.bytes_to_buffer(_vertex_pool.raw(), cc::make_pinned_data(cc::move(positions)).as_bytes(),
                               _placeholder.base_vertex * isize(sizeof(tg::pos3f)));
    up->upload.bytes_to_buffer(_index_pool.raw(), cc::make_pinned_data(cc::move(indices)).as_bytes(),
                               _placeholder.first_index * isize(sizeof(u32)));
    _ctx.submit_command_list(cc::move(up));
}

geometry_range mesh_manager::_allocate(isize vertex_count, isize triangle_count)
{
    _reclaim();

    auto base_vertex = _vertex_ranges.allocate(vertex_count);
    if (!base_vertex.has_value())
    {
        _grow_vertex_pool(_vertex_ranges.capacity() + vertex_count);
        base_vertex = _vertex_ranges.allocate(vertex_count);
    }
    auto first_index = _index_ranges.allocate(3 * triangle_count);
    if (!first_index.has_value())
    {
        _grow_index_pool(_index_ranges.capacity() + 3 * triangle_count);
        first_index = _index_ranges.allocate(3 * triangle_count);
    }
    CC_ASSERT(base_vertex.has_value() && first_index.has_value(), "a grown pool always fits the range it grew for");

    return {.base_vertex = base_vertex.value(),
            .vertex_count = vertex_count,
            .first_index = first_index.value(),
            .triangle_count = triangle_count};
}

void mesh_manager::_grow_vertex_pool(isize min_capacity)
{
    auto const old_capacity = _vertex_ranges.capacity();
    auto const capacity = grown_capacity(old_capacity, min_capacity);
    auto grown = _ctx.persistent.create_buffer<tg::pos3f>(capacity, pool_usage(_ctx));

    // Nothing streams into a pool, so all of it copies over; a mesh still streaming is copied in once it lands.
    auto cmd = _ctx.create_command_list();
    cmd->copy.buffer_data_region<tg::pos3f>({.src = _vertex_pool.raw(), .dst = grown.raw(), .count = old_capacity});
    _ctx.submit_command_list(cc::move(cmd));

    // The old buffer stays alive for the lists in flight that read it, as every dropped sg handle does.
    _vertex_pool = cc::move(grown);
    _vertex_ranges.grow(capacity);
    CC_LOG_INFO("grew the vertex pool to {} vertices", capacity);
}

void mesh_manager::_grow_index_pool(isize min_capacity)
{
    auto const old_capacity = _index_ranges.capacity();
    auto const capacity = grown_capacity(old_capacity, min_capacity);
    auto grown = _ctx.persistent.create_buffer<u32>(capacity, index_pool_usage(_ctx));

    // As `_grow_vertex_pool`, for the indices.
    auto cmd = _ctx.create_command_list();
    cmd->copy.buffer_data_region<u32>({.src = _index_pool.raw(), .dst = grown.raw(), .count = old_capacity});
    _ctx.submit_command_list(cc::move(cmd));

    _index_pool = cc::move(grown);
    _index_ranges.grow(capacity);
    CC_LOG_INFO("grew the index pool to {} indices", capacity);
}

void mesh_manager::on_remove(mesh_id id, mesh_record& record)
{
    // A still-streaming mesh never reached its ranges; dropping its entry cancels the transfers into its staging.
    _settling.erase(id);
    _retiring.push_back({.geometry = record.geometry, .evicted_in = _ctx.current_epoch()});
}

void mesh_manager::_reclaim()
{
    if (_retiring.empty() && _retiring_blocks.empty())
        return;

    auto const completed = u64(_ctx.completed_epoch());
    for (auto i = _retiring.size() - 1; i >= 0; --i)
    {
        auto const& r = _retiring[i];
        if (u64(r.evicted_in) > completed)
            continue;
        _vertex_ranges.free(r.geometry.base_vertex, r.geometry.vertex_count);
        _index_ranges.free(r.geometry.first_index, r.geometry.index_count());
        _retiring.remove_at_unordered(i);
    }
    for (auto i = _retiring_blocks.size() - 1; i >= 0; --i)
    {
        auto const& r = _retiring_blocks[i];
        if (u64(r.evicted_in) > completed)
            continue;
        _vertex_ranges.free(r.block.taken_offset, r.block.taken_count);
        _retiring_blocks.remove_at_unordered(i);
    }
}

mesh_manager::pool_block mesh_manager::allocate_block(isize count, isize alignment)
{
    CC_ASSERT(count > 0 && alignment >= 1, "a pool block needs a positive size and alignment");
    _ensure_pools();
    _reclaim();

    // Widened by what the alignment may skip, so any start the allocator hands back has an aligned one inside it.
    auto const taken_count = count + alignment - 1;
    auto taken = _vertex_ranges.allocate(taken_count);
    if (!taken.has_value())
    {
        _grow_vertex_pool(_vertex_ranges.capacity() + taken_count);
        taken = _vertex_ranges.allocate(taken_count);
    }
    CC_ASSERT(taken.has_value(), "a grown pool always fits the range it grew for");

    auto const offset = (taken.value() + alignment - 1) / alignment * alignment;
    return {.offset = offset, .count = count, .taken_offset = taken.value(), .taken_count = taken_count};
}

void mesh_manager::retire_block(pool_block const& block)
{
    if (block.taken_count > 0)
        _retiring_blocks.push_back({.block = block, .evicted_in = _ctx.current_epoch()});
}

geometry_range const& mesh_manager::placeholder_geometry()
{
    _ensure_pools();
    return _placeholder;
}

sg::buffer<tg::pos3f> const& mesh_manager::vertex_pool()
{
    _ensure_pools();
    return _vertex_pool;
}

sg::buffer<u32> const& mesh_manager::index_pool()
{
    _ensure_pools();
    return _index_pool;
}

sg::blas_handle const& mesh_manager::placeholder_blas()
{
    if (_placeholder_blas != nullptr)
        return _placeholder_blas;

    // A list of its own after the upload's, so the build sees the upload finished.
    _ensure_pools();
    auto build = _ctx.create_command_list();
    _placeholder_blas
        = build->raytracing.build_blas({{.vertices = _vertex_pool.raw(),
                                         .vertex_count = _placeholder.vertex_count,
                                         .vertex_offset_in_bytes = _placeholder.base_vertex * isize(sizeof(tg::pos3f)),
                                         .indices = _index_pool.raw(),
                                         .index_count = _placeholder.index_count(),
                                         .index_offset_in_bytes = _placeholder.first_index * isize(sizeof(u32))}});
    _ctx.submit_command_list(cc::move(build));
    return _placeholder_blas;
}

bool mesh_manager::_try_settle(mesh_id id, pending_mesh& p, std::unique_ptr<sg::command_list>& copies)
{
    if (!p.vertices.is_settled() || !p.indices.is_settled())
        return false;

    auto* const record = mutable_record(id);
    if (record == nullptr)
        return true; // evicted while streaming; `on_remove` normally takes the entry first

    // Settled without delivering — cancelled, or the transfer failed.
    // `failed` rather than leaving it pending, so nothing waits on it forever and the placeholder is known to be final.
    if (!p.vertices.is_complete() || !p.indices.is_complete())
    {
        record->state = residency::failed;
        return true;
    }

    // The staging is touched only now that its streams are observed settled, which is what the streaming contract asks.
    // Every later list reading the pools is submitted after this copy, so it is `complete` from here on.
    if (copies == nullptr)
        copies = _ctx.create_command_list();
    auto const& g = record->geometry;
    copies->copy.buffer_data_region<tg::pos3f>(
        {.src = p.staged_vertices.raw(), .dst = _vertex_pool.raw(), .count = g.vertex_count, .dst_offset = g.base_vertex});
    copies->copy.buffer_data_region<u32>(
        {.src = p.staged_indices.raw(), .dst = _index_pool.raw(), .count = g.index_count(), .dst_offset = g.first_index});
    record->state = residency::complete;
    return true;
}

sg::blas_handle const& mesh_manager::acquire_blas(sg::command_list& cmd, mesh_id id)
{
    auto* const record = mutable_record(id);
    CC_ASSERT(record != nullptr && record->state == residency::complete, "mesh_manager::acquire_blas needs a resident "
                                                                         "mesh whose geometry has landed");
    if (record->blas != nullptr)
        return record->blas;

    // `complete` means the copy into the pools was submitted, and `cmd` is the caller's to submit after this.
    auto const& g = record->geometry;
    try
    {
        record->blas = cmd.raytracing.build_blas({{.vertices = _vertex_pool.raw(),
                                                   .vertex_count = g.vertex_count,
                                                   .vertex_offset_in_bytes = g.base_vertex * isize(sizeof(tg::pos3f)),
                                                   .indices = _index_pool.raw(),
                                                   .index_count = g.index_count(),
                                                   .index_offset_in_bytes = g.first_index * isize(sizeof(u32))}});
    }
    catch (sg::allocation_exception const& e)
    {
        // A pool with a ceiling, as webgpu's is, can be full of a scene that was just left and is still retiring.
        // The mesh traces as the placeholder meanwhile, and asks again on a later frame.
        if (!_told_blas_pool_full)
            CC_LOG_INFO("a mesh's acceleration structure does not fit yet, so it stands in until space frees: {}",
                        e.message());
        _told_blas_pool_full = true;
        return placeholder_blas();
    }

    // Only now is the acceleration structure's own cost known, which is the one thing a record cannot size at insert.
    add_bytes(id, record->blas->size_in_bytes());
    return record->blas;
}
bool mesh_manager::_emitter_cdf_kernels_ready()
{
    if (_cdf_failed)
        return false;
    if (!_cdf_blocks.is_valid())
    {
        _cdf_blocks = shaders::emitter_cdf::blocks.acquire_pipeline(_ctx);
        _cdf_total = shaders::emitter_cdf::total.acquire_pipeline(_ctx);
        _world_blocks = shaders::emitter_cdf::world_blocks.acquire_pipeline(_ctx);
        _world_total = shaders::emitter_cdf::world_total.acquire_pipeline(_ctx);
        (void)_ctx.backlog.start(_cdf_blocks);
        (void)_ctx.backlog.start(_cdf_total);
        (void)_ctx.backlog.start(_world_blocks);
        (void)_ctx.backlog.start(_world_total);
        return false;
    }
    auto const kernels = cc::fixed_array<sg::async_compute_pipeline const*, 4>{&_cdf_blocks, &_cdf_total,
                                                                               &_world_blocks, &_world_total};
    for (auto const* k : kernels)
        if (!(*k)->is_ready())
            return false;

    auto built = true;
    for (auto const* k : kernels)
    {
        auto const* const value = (*k)->try_value();
        built = built && value != nullptr && *value != nullptr;
    }
    if (!built)
    {
        CC_LOG_ERROR("the kernels that build an emitter's area CDF did not build, so emissive meshes light nothing");
        _cdf_failed = true;
        return false;
    }
    return true;
}

sg::buffer<f32> const* mesh_manager::acquire_emitter_cdf(sg::command_list& cmd, mesh_id id)
{
    auto* const record = mutable_record(id);
    CC_ASSERT(record != nullptr && record->state == residency::complete, "mesh_manager::acquire_emitter_cdf needs a "
                                                                         "resident mesh whose geometry has landed");
    if (record->emitter_cdf.raw() != nullptr)
        return &record->emitter_cdf;
    if (!_emitter_cdf_kernels_ready())
        return nullptr;

    auto const& g = record->geometry;
    auto const triangles = u32(g.triangle_count);
    auto const blocks = emitter_cdf_block_count(g.triangle_count);
    auto cdf = _ctx.persistent.create_buffer<f32>(
        isize(triangles) + isize(blocks) + 1,
        sg::buffer_usage::readwrite_buffer | sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_src);

    auto const group = _ctx.transient.create_binding_group(
        cmd, _ctx.cached.acquire_binding_group_layout<shaders::emitter_cdf::emitter_cdf_io>(),
        shaders::emitter_cdf::emitter_cdf_io{.vertices = _vertex_pool.reinterpret_as<byte>().as_readonly_buffer(),
                                             .indices = _index_pool.reinterpret_as<byte>().as_readonly_buffer(),
                                             .cdf = cdf.as_readwrite_buffer()});
    auto const shape = shaders::emitter_cdf::emitter_cdf_shape{.triangle_count = triangles,
                                                               .block_count = blocks,
                                                               .base_vertex = u32(g.base_vertex),
                                                               .first_index = u32(g.first_index)}
                           .to_block();

    // Two dispatches over one buffer, the second reading what the first wrote: the blocks, then the mesh over them.
    cmd.compute.bind_pipeline(**_cdf_blocks->try_value());
    cmd.compute.bind_group(0, *group);
    cmd.compute.set_inline_constants(shape);
    cmd.compute.dispatch_threads(int(blocks));
    cmd.compute.bind_pipeline(**_cdf_total->try_value());
    cmd.compute.bind_group(0, *group);
    cmd.compute.set_inline_constants(shape);
    cmd.compute.dispatch_threads(1);

    add_bytes(id, cdf.size_in_bytes());
    record->emitter_cdf = cc::move(cdf);
    return &record->emitter_cdf;
}

cc::optional<f32> mesh_manager::acquire_world_area(sg::command_list& cmd, mesh_id id, tg::mat3f const& linear)
{
    auto* const record = mutable_record(id);
    CC_ASSERT(record != nullptr && record->state == residency::complete, "mesh_manager::acquire_world_area needs a "
                                                                         "resident mesh whose geometry has landed");
    auto& areas = record->world_areas;
    for (auto i = isize(0); i < areas.size(); ++i)
    {
        if (areas[i].linear != linear)
            continue;
        // Asked for again, so it moves to the back, the last to be dropped.
        if (i + 1 < areas.size())
        {
            auto moved = cc::move(areas[i]);
            areas.remove_at(i);
            areas.push_back(cc::move(moved));
        }
        auto& w = areas.back();
        if (!w.area.has_value() && w.pending.is_valid() && w.pending.is_ready())
        {
            auto const sums = w.pending.try_get_data();
            if (sums.has_value() && !sums.value().empty())
                w.area = sums.value()[sums.value().size() - 1];
            w.pending = {};
        }
        return w.area;
    }
    if (!_emitter_cdf_kernels_ready())
        return {};

    auto const& g = record->geometry;
    auto const triangles = u32(g.triangle_count);
    auto const blocks = emitter_cdf_block_count(g.triangle_count);
    auto sums = _ctx.persistent.create_buffer<f32>(isize(blocks) + 1,
                                                   sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const group = _ctx.transient.create_binding_group(
        cmd, _ctx.cached.acquire_binding_group_layout<shaders::emitter_cdf::emitter_cdf_io>(),
        shaders::emitter_cdf::emitter_cdf_io{.vertices = _vertex_pool.reinterpret_as<byte>().as_readonly_buffer(),
                                             .indices = _index_pool.reinterpret_as<byte>().as_readonly_buffer(),
                                             .cdf = sums.as_readwrite_buffer()});
    auto const column = [&](tg::vec3f axis)
    {
        auto const c = linear * axis;
        return tg::vec4f(c[0], c[1], c[2], 0.0f);
    };
    auto const shape = shaders::emitter_cdf::emitter_cdf_shape{.column_x = column(tg::vec3f(1, 0, 0)),
                                                               .column_y = column(tg::vec3f(0, 1, 0)),
                                                               .column_z = column(tg::vec3f(0, 0, 1)),
                                                               .triangle_count = triangles,
                                                               .block_count = blocks,
                                                               .base_vertex = u32(g.base_vertex),
                                                               .first_index = u32(g.first_index)}
                           .to_block();

    // The blocks, then the instance over them, as the CDF's build runs.
    cmd.compute.bind_pipeline(**_world_blocks->try_value());
    cmd.compute.bind_group(0, *group);
    cmd.compute.set_inline_constants(shape);
    cmd.compute.dispatch_threads(int(blocks));
    cmd.compute.bind_pipeline(**_world_total->try_value());
    cmd.compute.bind_group(0, *group);
    cmd.compute.set_inline_constants(shape);
    cmd.compute.dispatch_threads(1);

    if (areas.size() >= max_world_areas)
        areas.remove_at(0);
    areas.push_back({.linear = linear, .pending = cmd.download.data_from_buffer(sums), .area = {}});
    return {};
}

isize mesh_manager::collect_settled()
{
    _reclaim();
    if (_settling.empty())
        return 0;

    auto finished = isize(0);
    auto keep = cc::map<mesh_id, pending_mesh>();
    auto copies = std::unique_ptr<sg::command_list>();
    for (auto&& [id, p] : _settling)
    {
        if (_try_settle(id, p, copies))
            ++finished;
        else
            keep[id] = cc::move(p);
    }
    if (copies != nullptr)
        _ctx.submit_command_list(cc::move(copies));

    _settling = cc::move(keep);
    return finished;
}

void mesh_manager::wait_for_settled()
{
    if (_settling.empty())
        return;

    // Promoted first: with the automatic waits back on, a list recorded after this needs no ordering of its own.
    for (auto&& [id, p] : _settling)
    {
        (void)id;
        p.vertices.promote_to_async();
        p.indices.promote_to_async();
        (void)cc::try_async_blocking_get(p.vertices.completion());
        (void)cc::try_async_blocking_get(p.indices.completion());
    }
    (void)collect_settled();
}

quadric_manager quadric_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto manager = quadric_manager(ctx);
    manager.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return manager;
}

quadric_set_id quadric_manager::acquire(quadric_data const& set, mesh_manager& meshes)
{
    if (auto const id = find_by_hash(set.hash); id.has_value())
        return id.value();

    auto const primitives = set.primitives;
    CC_ASSERT(!primitives.empty(), "quadric_manager::acquire expects at least one primitive");

    // The split the authored form cannot avoid: a primitive carries its box beside it, and the two go to different
    // blocks — the boxes to the acceleration structure's build input, the rest to what an intersection reads.
    // It happens HERE rather than at authoring time because it is only paid on a miss, which is what lets a batch be
    // re-acquired every frame for the cost of a hash lookup.
    static_assert(sizeof(quadric_gpu) == primitive_elements * sizeof(tg::pos3f), "a record must fill whole pool "
                                                                                 "elements");
    static_assert(sizeof(tg::aabb3f) == box_elements * sizeof(tg::pos3f), "a box must fill whole pool elements");
    auto const n = primitives.size();
    auto packed = cc::vector<tg::pos3f>::create_uninitialized(n * primitive_elements);
    auto boxes = cc::vector<tg::pos3f>::create_uninitialized(n * box_elements);
    for (auto i = isize(0); i < n; ++i)
    {
        auto const record = quadric_gpu::of(primitives[i]);
        cc::memcpy(packed.data() + i * primitive_elements, &record, sizeof(quadric_gpu));
        cc::memcpy(boxes.data() + i * box_elements, &primitives[i].bounds, sizeof(tg::aabb3f));
    }

    // Blocks of the vertex pool, and staging the payload streams into, as for a mesh: the bytes are not sent here, so
    // a large batch does not stall the frame that asked for it.
    auto const primitive_block = meshes.allocate_block(packed.size());
    auto const box_block = meshes.allocate_block(boxes.size(), box_elements);
    auto pending = pending_set{
        .staged_primitives = _ctx.persistent.create_buffer<tg::pos3f>(packed.size(), staging_usage),
        .staged_aabbs = _ctx.persistent.create_buffer<tg::pos3f>(boxes.size(), staging_usage),
        .bytes = (packed.size() + boxes.size()) * isize(sizeof(tg::pos3f)),
    };
    pending.primitives = _ctx.stream.data_to_buffer(pending.staged_primitives, cc::make_pinned_data(cc::move(packed)));
    pending.aabbs = _ctx.stream.data_to_buffer(pending.staged_aabbs, cc::make_pinned_data(cc::move(boxes)));
    pending.primitives.set_priority(impl::geometry_stream_priority);
    pending.aabbs.set_priority(impl::geometry_stream_priority);

    // The BLAS is not charged yet, since it is not built: `acquire_blas` adds its size once it is.
    auto const id = insert(set.hash,
                           {.state = residency::pending,
                            .primitives = primitive_block,
                            .aabbs = box_block,
                            .primitive_count = n,
                            .bounds = set.bounds},
                           pending.bytes);
    _settling[id] = cc::move(pending);
    return id;
}

void quadric_manager::on_remove(quadric_set_id id, quadric_set_record& record)
{
    // A still-streaming batch never reached its blocks; dropping its entry cancels the transfers into its staging.
    _settling.erase(id);
    _released.push_back(record.primitives);
    _released.push_back(record.aabbs);
}

bool quadric_manager::_try_settle(quadric_set_id id,
                                  pending_set& p,
                                  mesh_manager& meshes,
                                  std::unique_ptr<sg::command_list>& copies)
{
    if (!p.primitives.is_settled() || !p.aabbs.is_settled())
        return false;

    auto* const record = mutable_record(id);
    if (record == nullptr)
        return true; // evicted while streaming; dropping the entry cancels whatever is left

    // Settled without delivering — cancelled, or the transfer failed.
    // `failed` rather than leaving it pending, so nothing waits on it forever and the placeholder is known to be final.
    if (!p.primitives.is_complete() || !p.aabbs.is_complete())
    {
        record->state = residency::failed;
        return true;
    }

    // As a mesh's: the staging is touched only now that its streams are observed settled, and every later list reading
    // the pool is submitted after this copy.
    if (copies == nullptr)
        copies = _ctx.create_command_list();
    auto const& pool = meshes.vertex_pool();
    copies->copy.buffer_data_region<tg::pos3f>({.src = p.staged_primitives.raw(),
                                                .dst = pool.raw(),
                                                .count = record->primitives.count,
                                                .dst_offset = record->primitives.offset});
    copies->copy.buffer_data_region<tg::pos3f>(
        {.src = p.staged_aabbs.raw(), .dst = pool.raw(), .count = record->aabbs.count, .dst_offset = record->aabbs.offset});
    record->state = residency::complete;
    return true;
}

sg::blas_handle const& quadric_manager::acquire_blas(sg::command_list& cmd, quadric_set_id id, mesh_manager& meshes)
{
    auto* const record = mutable_record(id);
    CC_ASSERT(record != nullptr && record->state == residency::complete, "quadric_manager::acquire_blas needs a "
                                                                         "resident batch whose blocks have landed");
    if (record->blas != nullptr)
        return record->blas;

    // Only the BOXES are the build input; the records are an intersection's and the hardware never sees them.
    // `tg::aabb3f` is already the six-float min-then-max layout a procedural BLAS wants, so the stride is its own size.
    auto const geometry = sg::blas_aabbs{.aabbs = meshes.vertex_pool().raw(),
                                         .aabb_count = record->primitive_count,
                                         .aabb_stride_in_bytes = isize(sizeof(tg::aabb3f)),
                                         .aabb_offset_in_bytes = record->aabbs.offset * isize(sizeof(tg::pos3f))};

    // `complete` was observed before this call, as the streaming contract asks of the list `cmd` is submitted as.
    record->blas = cmd.raytracing.build_blas(cc::span<sg::blas_aabbs const>(&geometry, 1));

    // Only now is the acceleration structure's own cost known, which is the one thing a record cannot size at insert.
    add_bytes(id, record->blas->size_in_bytes());
    return record->blas;
}

isize quadric_manager::collect_settled(mesh_manager& meshes)
{
    for (auto const& block : _released)
        meshes.retire_block(block);
    _released.clear();
    if (_settling.empty())
        return 0;

    auto finished = isize(0);
    auto keep = cc::map<quadric_set_id, pending_set>();
    auto copies = std::unique_ptr<sg::command_list>();
    for (auto&& [id, p] : _settling)
    {
        if (_try_settle(id, p, meshes, copies))
            ++finished;
        else
            keep[id] = cc::move(p);
    }
    if (copies != nullptr)
        _ctx.submit_command_list(cc::move(copies));

    _settling = cc::move(keep);
    return finished;
}

void quadric_manager::wait_for_settled(mesh_manager& meshes)
{
    if (_settling.empty())
        return;

    // Promoted first: with the automatic waits back on, a list recorded after this needs no ordering of its own.
    for (auto&& [id, p] : _settling)
    {
        (void)id;
        p.primitives.promote_to_async();
        p.aabbs.promote_to_async();
        (void)cc::try_async_blocking_get(p.primitives.completion());
        (void)cc::try_async_blocking_get(p.aabbs.completion());
    }
    (void)collect_settled(meshes);
}

texture_manager texture_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto m = texture_manager(ctx);
    m.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return m;
}

void texture_manager::mark_mips_complete(texture_id id)
{
    auto* const record = mutable_record(id);
    if (record == nullptr)
        return;
    record->uploaded_mips = record->total_mips;
    record->state = residency::complete;
}

namespace
{
/// What a chain of `levels` levels of `format` from `width` x `height` occupies, block-compressed formats included.
isize chain_bytes_of(sg::pixel_format format, i32 width, i32 height, i32 levels)
{
    auto const extent = isize(sg::format_block_extent(format));
    auto const block = isize(sg::format_block_size(format));
    auto bytes = isize(0);
    for (auto mip = i32(0); mip < levels; ++mip)
    {
        auto const w = cc::max(isize(1), isize(width) >> mip);
        auto const h = cc::max(isize(1), isize(height) >> mip);
        bytes += (w + extent - 1) / extent * ((h + extent - 1) / extent) * block;
    }
    return bytes;
}
} // namespace

void texture_manager::replace_with_compressed(texture_id id, sg::texture_2d compressed)
{
    auto* const record = mutable_record(id);
    if (record == nullptr)
        return;

    auto const& old = record->texture;
    auto const before = chain_bytes_of(old.format(), old.width(), old.height(), record->total_mips);
    auto const after = chain_bytes_of(compressed.format(), compressed.width(), compressed.height(), record->total_mips);
    record->texture = cc::move(compressed);
    record->compressed = true;
    add_bytes(id, after - before);

    // The summary read the source pixels, and the encoding's error may lift a texel off the bound that proved it dark.
    // It still estimates the mean, which is all selection needs of it.
    if (record->summary.has_value())
        record->summary.value().exact = false;
}

sg::texture_2d_description adoptable_texture(sg::context& ctx, sg::pixel_format format, i32 width, i32 height)
{
    CC_ASSERT(width > 0 && height > 0, "a texture needs a positive extent");

    // Which routine can fill the levels an upload did not supply is a property of the FORMAT, and the usage it needs
    // is fixed at creation — so the choice is made here rather than where the generation is recorded.
    // An sRGB format carries no typed UAV and a compressed one is neither writable nor renderable, which leaves the
    // compressed case with no path at all; it allocates only the levels its file supplied.
    // An image format outside the portable set is writable only where the device grants extended image formats.
    auto const compute_mips = sg::supports_typed_uav(format)
                           && (sg::is_portable_image_format(format) || ctx.supports(sg::feature::extended_image_formats));
    auto const raster_mips = !compute_mips && sg::is_render_target_format(format);

    // The full chain is allocated up front even when only the base level is supplied, so generating the rest
    // later fills this texture in place rather than replacing it.
    // A format neither routine can fill has room for the levels it is given only, which is 1 here.
    auto usage = sg::texture_usage::texture | sg::texture_usage::copy_dst;
    if (compute_mips)
        usage = usage | sg::texture_usage::image;
    else if (raster_mips)
        usage = usage | sg::texture_usage::render_target;

    return {.format = format,
            .width = width,
            .height = height,
            .mip_levels = compute_mips || raster_mips ? impl::mip_count_of(width, height) : 1,
            .usage = usage};
}

texture_id texture_manager::acquire(texture_data const& texture)
{
    if (auto const resident = find_by_hash(texture.hash); resident.has_value())
        return resident.value();

    CC_ASSERT(texture.width > 0 && texture.height > 0, "a texture needs a positive extent");
    CC_ASSERT(texture.mip_count >= 1, "a texture carries at least its base level");

    // The budget is charged for the whole allocated chain rather than the levels supplied so far.
    // A record's byte size is fixed at insert, so `mark_mips_complete` could not revise it afterwards, and a
    // completed texture would sit in the budget at three quarters of what it costs.
    auto const chain_bytes_of = [&](i32 levels)
    {
        auto bytes = isize(0);
        for (auto mip = i32(0); mip < levels; ++mip)
            bytes += impl::mip_byte_size(texture.format, texture.width, texture.height, mip);
        return bytes;
    };

    auto const description = adoptable_texture(_ctx, texture.format, texture.width, texture.height);

    if (texture.gpu.has_value())
    {
        auto const& gpu = texture.gpu.value();
        CC_ASSERT(gpu.raw()->format() == texture.format && gpu.width() == texture.width && gpu.height() == texture.height,
                  "an adopted texture has the shape its data names");
        CC_ASSERT(gpu.mip_levels() == description.mip_levels && gpu.raw()->usage().has_all(description.usage),
                  "an adopted texture is created from adoptable_texture's description");

        // Nothing streams: an empty transfer list is settled and delivered, so the next `collect_settled` reports it
        // as landed, and its follow-up work is queued as an upload's is.
        auto const id = insert(texture.hash,
                               {.texture = gpu,
                                .state = residency::pending,
                                .uploaded_mips = texture.mip_count,
                                .total_mips = description.mip_levels,
                                .summary = texture.summary},
                               chain_bytes_of(description.mip_levels));
        _settling[id] = {};
        return id;
    }

    // A format with no mip routine keeps the levels it was given, whatever their number.
    auto const total_mips = description.mip_levels > 1 ? description.mip_levels : texture.mip_count;
    CC_ASSERT(texture.mip_count <= total_mips, "more mips supplied than the extent has");

    auto gpu = _ctx.persistent.create_texture_2d({.format = texture.format,
                                                  .width = texture.width,
                                                  .height = texture.height,
                                                  .mip_levels = total_mips,
                                                  .usage = description.usage});

    // Every supplied level as its own transfer, each keeping a pin into the caller's pixels — so the payload outlives
    // this call without the manager holding a copy of it.
    // Below the geometry priority: a gray model beats a floating albedo map.
    auto transfers = cc::vector<sg::stream_upload_handle>();
    auto offset = isize(0);
    for (auto mip = i32(0); mip < texture.mip_count; ++mip)
    {
        auto const size = impl::mip_byte_size(texture.format, texture.width, texture.height, mip);
        CC_ASSERT(offset + size <= texture.pixels.span().size(), "texture pixels are shorter than the mips they claim");
        transfers.push_back(_ctx.stream.bytes_to_texture(
            gpu.raw(), texture.pixels.subdata({.offset = offset, .size = size}), {.mip_level = mip}));
        transfers.back().set_priority(impl::texture_stream_priority);
        offset += size;
    }

    // Summarized while the pixels are at hand.
    // Every texture is, since which ones a material reads as an emission is not known here; the scan is capped and cheap.
    auto const summary = summarize_texture(texture);

    auto const chain_bytes = chain_bytes_of(total_mips);

    auto const id = insert(texture.hash,
                           {.texture = cc::move(gpu),
                            .state = residency::pending,
                            .uploaded_mips = texture.mip_count,
                            .total_mips = total_mips,
                            .summary = summary},
                           chain_bytes);

    _settling[id] = cc::move(transfers);
    return id;
}

isize texture_manager::collect_settled(cc::vector<texture_id>& newly_resident)
{
    if (_settling.empty())
        return 0;

    auto finished = isize(0);
    auto keep = cc::map<texture_id, cc::vector<sg::stream_upload_handle>>();

    for (auto&& [id, transfers] : _settling)
    {
        auto settled = true;
        auto delivered = true;
        for (auto const& t : transfers)
        {
            settled = settled && t.is_settled();
            delivered = delivered && t.is_complete();
        }

        if (!settled)
        {
            keep[id] = cc::move(transfers);
            continue;
        }

        ++finished;
        auto* const record = mutable_record(id);
        if (record == nullptr)
            continue; // evicted while streaming

        if (!delivered)
        {
            record->state = residency::failed;
            continue;
        }

        // Whether it is done is the shape's answer, not the upload's: a texture given every level it has room for
        // needs no follow-up, one given fewer is waiting on mip generation.
        //
        // A texture whose data carried fewer mips than its shape allows is sampleable but not finished; filling the
        // rest is the owning manager's to schedule, which is why the id is reported rather than just recorded.
        record->state = record->uploaded_mips >= record->total_mips ? residency::complete : residency::base_resident;
        newly_resident.push_back(id);
    }

    _settling = cc::move(keep);
    return finished;
}

void texture_manager::wait_for_settled(cc::vector<texture_id>& newly_resident)
{
    for (auto&& [id, transfers] : _settling)
    {
        (void)id;
        for (auto& t : transfers)
        {
            t.promote_to_async();
            (void)cc::try_async_blocking_get(t.completion());
        }
    }
    (void)collect_settled(newly_resident);
}

namespace
{
// readonly_buffer so a shader reads the pool as `bytes`; copy_dst for the writes into it; copy_src for its growth and a readback.
constexpr auto attribute_pool_usage
    = sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst | sg::buffer_usage::copy_src;

// What an attribute streams into before it is copied into its range.
constexpr auto attribute_staging_usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

/// The attribute pool's first size.
constexpr isize initial_attribute_pool_bytes = 1024 * 1024;

/// Every range starts on 16 bytes, so a `load4` of an element whose format is 16-byte aligned stays aligned in the pool.
constexpr isize attribute_alignment = 16;
} // namespace

attribute_manager attribute_manager::create(sg::context& ctx, manager_config const& cfg)
{
    auto manager = attribute_manager(ctx);
    manager.set_limits(cfg.budget.max_bytes, cfg.budget.max_idle_epochs);
    return manager;
}

attribute_id attribute_manager::acquire(mesh_attribute const& attribute)
{
    if (auto const resident = find_by_hash(attribute.hash); resident.has_value())
        return resident.value();

    auto const bytes = attribute.data.span();
    CC_ASSERT(!bytes.empty(), "an attribute a material reads must carry elements");

    auto staging = _ctx.persistent.create_buffer<byte>(bytes.size(), attribute_staging_usage);
    auto transfer = _ctx.stream.bytes_to_buffer(staging.raw(), attribute.data);
    transfer.set_priority(impl::attribute_stream_priority);

    // Summarized while the payload is at hand, since it is the stream's from here on.
    auto const id = insert(attribute.hash,
                           {.state = residency::pending,
                            .size_bytes = bytes.size(),
                            .format = attribute.format,
                            .frequency = attribute.frequency,
                            .element_count = attribute.element_count(),
                            .summary = summarize_elements(bytes, attribute.format, attribute.element_count())},
                           bytes.size());

    _settling[id] = {.staging = cc::move(staging), .transfer = cc::move(transfer)};
    return id;
}

attribute_id attribute_manager::acquire_inline(mesh_attribute const& attribute)
{
    if (auto const resident = find_by_hash(attribute.hash); resident.has_value())
        return resident.value();

    auto const bytes = attribute.data.span();
    CC_ASSERT(!bytes.empty(), "an attribute a material reads must carry elements");

    auto const id = insert(attribute.hash,
                           {.state = residency::pending,
                            .size_bytes = bytes.size(),
                            .format = attribute.format,
                            .frequency = attribute.frequency,
                            .element_count = attribute.element_count(),
                            .summary = summarize_elements(bytes, attribute.format, attribute.element_count())},
                           bytes.size());
    _inline_bytes[id] = attribute.data;
    return id;
}

isize attribute_manager::_allocate(sg::command_list& cmd, isize bytes)
{
    _reclaim();
    auto const size = cc::align_up(bytes, attribute_alignment);
    if (_pool.raw() == nullptr)
    {
        auto const capacity = grown_capacity(initial_attribute_pool_bytes / 2, size);
        _pool = _ctx.persistent.create_buffer<byte>(capacity, attribute_pool_usage);
        _ranges.grow(capacity);
    }
    auto offset = _ranges.allocate(size);
    if (!offset.has_value())
    {
        // Recorded on `cmd`, after every write into the old pool this manager already recorded or submitted.
        auto const old_capacity = _ranges.capacity();
        auto const capacity = grown_capacity(old_capacity, old_capacity + size);
        auto grown = _ctx.persistent.create_buffer<byte>(capacity, attribute_pool_usage);
        cmd.copy.buffer_data_region<byte>({.src = _pool.raw(), .dst = grown.raw(), .count = old_capacity});

        // The old buffer stays alive for the lists in flight that read it, as every dropped sg handle does.
        _pool = cc::move(grown);
        _ranges.grow(capacity);
        offset = _ranges.allocate(size);
        CC_LOG_INFO("grew the attribute pool to {} bytes", capacity);
    }
    CC_ASSERT(offset.has_value(), "a grown pool always fits the range it grew for");
    return offset.value();
}

void attribute_manager::record_inline_uploads(sg::command_list& cmd)
{
    for (auto&& [id, bytes] : _inline_bytes)
    {
        // evicted before a command list came for it: nothing is left to write into
        auto* const record = mutable_record(id);
        if (record == nullptr)
            continue;
        record->offset = _allocate(cmd, record->size_bytes);
        cmd.upload.bytes_to_buffer(_pool.raw(), bytes.span(), record->offset);
        record->state = residency::complete;
    }
    _inline_bytes.clear();
}

isize attribute_manager::collect_settled(sg::command_list* cmd)
{
    _reclaim();
    if (_settling.empty())
        return 0;

    auto finished = isize(0);
    auto keep = cc::map<attribute_id, pending_attribute>();
    auto own = std::unique_ptr<sg::command_list>();
    for (auto&& [id, p] : _settling)
    {
        if (!p.transfer.is_settled())
        {
            keep[id] = cc::move(p);
            continue;
        }
        ++finished;

        auto* const record = mutable_record(id);
        if (record == nullptr)
            continue;
        if (!p.transfer.is_complete())
        {
            record->state = residency::failed;
            continue;
        }
        if (cmd == nullptr && own == nullptr)
            own = _ctx.create_command_list();
        auto& copies = cmd != nullptr ? *cmd : *own;
        record->offset = _allocate(copies, record->size_bytes);
        copies.copy.buffer_data_region<byte>(
            {.src = p.staging.raw(), .dst = _pool.raw(), .count = record->size_bytes, .dst_offset = record->offset});
        record->state = residency::complete;
    }
    if (own != nullptr)
        _ctx.submit_command_list(cc::move(own));

    _settling = cc::move(keep);
    return finished;
}

void attribute_manager::wait_for_settled()
{
    for (auto&& [id, p] : _settling)
    {
        (void)id;
        p.transfer.promote_to_async();
        (void)cc::try_async_blocking_get(p.transfer.completion());
    }
    (void)collect_settled();
}

void attribute_manager::on_remove(attribute_id id, attribute_record& record)
{
    // A still-streaming attribute never reached the pool; dropping its entry cancels the transfer into its staging.
    _settling.erase(id);
    _inline_bytes.erase(id);
    if (record.offset >= 0)
        _retiring.push_back({.offset = record.offset,
                             .size_bytes = cc::align_up(record.size_bytes, attribute_alignment),
                             .evicted_in = _ctx.current_epoch()});
}

void attribute_manager::_reclaim()
{
    if (_retiring.empty())
        return;

    auto const completed = u64(_ctx.completed_epoch());
    for (auto i = _retiring.size() - 1; i >= 0; --i)
    {
        auto const& r = _retiring[i];
        if (u64(r.evicted_in) > completed)
            continue;
        _ranges.free(r.offset, r.size_bytes);
        _retiring.remove_at_unordered(i);
    }
}

} // namespace sv
