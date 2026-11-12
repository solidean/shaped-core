#include "software_trace.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg::impl
{
namespace
{
constexpr u32 leaf_flag = 0x80000000u;
constexpr u32 max_leaf_primitives = 4;
constexpr isize unit_bytes = 16;
constexpr isize words_per_unit = 4;

/// The first pool, 1 MiB.
constexpr isize initial_capacity_units = isize(1) << 16;
/// A descriptor's range holds the whole pool, and a device's maxStorageBufferRange is often 1 GiB.
constexpr isize max_capacity_units = isize(1) << 26;

constexpr float huge = 3.0e38f;

[[nodiscard]] float magnitude(float f)
{
    return f < 0 ? -f : f;
}

[[nodiscard]] u32 float_bits(float f)
{
    return cc::bit_cast<u32>(f);
}

struct float3
{
    float x = 0;
    float y = 0;
    float z = 0;

    [[nodiscard]] float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};

struct bounds
{
    float lo[3] = {huge, huge, huge};
    float hi[3] = {-huge, -huge, -huge};

    void add(float3 const& p)
    {
        for (auto i = 0; i < 3; ++i)
        {
            lo[i] = cc::min(lo[i], p[i]);
            hi[i] = cc::max(hi[i], p[i]);
        }
    }
    void add(bounds const& b)
    {
        for (auto i = 0; i < 3; ++i)
        {
            lo[i] = cc::min(lo[i], b.lo[i]);
            hi[i] = cc::max(hi[i], b.hi[i]);
        }
    }
};

/// The three floats at `offset` of `bytes`.
[[nodiscard]] float3 read_float3(cc::span<byte const> bytes, isize offset, char const* what)
{
    CC_ASSERTF(offset >= 0 && offset + 12 <= bytes.size(), "build: {} ends at byte {}, past the {} bytes given", what,
               offset + 12, bytes.size());
    auto out = float3();
    cc::memcpy(&out, bytes.data() + offset, 12);
    return out;
}

/// The region's words, its header and nodes written, and every box left to `fill_boxes`.
[[nodiscard]] cc::vector<u32> region_words(acceleration_tree const& tree,
                                           isize units,
                                           u32 first_record_unit,
                                           u32 count,
                                           u32 kind_word)
{
    auto words = cc::vector<u32>::create_filled(units * words_per_unit, u32(0));
    words[0] = tree.node_count;
    words[1] = first_record_unit;
    words[2] = count;
    words[3] = kind_word;
    for (isize i = 0; i < tree.node_words.size(); ++i)
        words[words_per_unit + i] = tree.node_words[i];
    return words;
}

void store_box(cc::span<u32> words, u32 node, bounds const& box)
{
    auto const at = (1 + 2 * isize(node)) * words_per_unit;
    for (auto i = 0; i < 3; ++i)
    {
        words[at + i] = float_bits(box.lo[i]);
        words[at + words_per_unit + i] = float_bits(box.hi[i]);
    }
}

[[nodiscard]] bounds load_box(cc::span<u32 const> words, u32 node)
{
    auto const at = (1 + 2 * isize(node)) * words_per_unit;
    auto out = bounds();
    for (auto i = 0; i < 3; ++i)
    {
        out.lo[i] = cc::bit_cast<float>(words[at + i]);
        out.hi[i] = cc::bit_cast<float>(words[at + words_per_unit + i]);
    }
    return out;
}

/// The leaves from `primitive(j)`'s box, then each level from its children; gives the root's box.
template <class Primitive>
[[nodiscard]] bounds fill_boxes(cc::span<u32> words, acceleration_tree const& tree, Primitive const& primitive)
{
    for (auto node = tree.first_leaf; node < tree.first_leaf + tree.leaf_count; ++node)
    {
        auto const at = (1 + 2 * isize(node)) * words_per_unit;
        auto const first = words[at + 3];
        auto const count = words[at + 7] & ~leaf_flag;
        auto box = bounds();
        for (auto k = u32(0); k < count; ++k)
            box.add(primitive(first + k));
        store_box(words, node, box);
    }
    for (auto const& level : tree.levels)
        for (auto node = level.first; node < level.first + level.count; ++node)
        {
            auto const at = (1 + 2 * isize(node)) * words_per_unit;
            auto box = load_box(words, words[at + 3]);
            box.add(load_box(words, words[at + 7]));
            store_box(words, node, box);
        }
    return load_box(words, 0);
}

/// `box` under the row-major 3x4 `rows`, widened since the transform rounds and the box must still hold every point.
/// The rounding scales with the terms summed rather than with the sum, which cancellation can make small.
[[nodiscard]] bounds moved_box(bounds const& box, float const* rows)
{
    auto out = bounds();
    for (auto row = 0; row < 3; ++row)
    {
        auto const* const m = rows + 4 * row;
        auto center = m[3];
        auto extent = 0.0f;
        auto terms = magnitude(m[3]);
        for (auto c = 0; c < 3; ++c)
        {
            auto const box_center = 0.5f * (box.lo[c] + box.hi[c]);
            center += m[c] * box_center;
            extent += magnitude(m[c]) * 0.5f * (box.hi[c] - box.lo[c]);
            terms += magnitude(m[c]) * magnitude(box_center);
        }
        auto const pad = 1.0e-6f * (terms + extent);
        out.lo[row] = center - extent - pad;
        out.hi[row] = center + extent + pad;
    }
    return out;
}

[[nodiscard]] software_region written_region(software_acceleration_pool& pool,
                                             u32 unit,
                                             isize units,
                                             cc::span<u32 const> words,
                                             bounds const& box,
                                             bool is_moving)
{
    pool.write(unit, words);
    auto region = software_region{.pool = &pool, .unit = unit, .units = u32(units), .is_moving = is_moving};
    for (auto i = 0; i < 3; ++i)
    {
        region.box_min[i] = box.lo[i];
        region.box_max[i] = box.hi[i];
    }
    return region;
}
} // namespace

acceleration_tree lay_out_acceleration_tree(u32 primitive_count)
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
    auto made_levels = cc::vector<acceleration_tree::level>();
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

    auto tree = acceleration_tree{.node_count = made, .first_leaf = made - leaves, .leaf_count = leaves};
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

void append_instance_words(cc::vector<u32>& words, tlas_instance const& instance, u32 blas_unit, u32 index, bool tlas_moves)
{
    float world_to_object[12];
    invert_affine(instance.transform, world_to_object);
    for (auto const f : world_to_object)
        words.push_back(float_bits(f));
    for (auto const f : instance.transform)
        words.push_back(float_bits(f));

    auto instance_flags = u32(0);
    if (instance.opaque_override.has_value())
        instance_flags |= instance.opaque_override.value() ? 1u : 2u;
    instance_flags |= u32(instance.cull_mode) << 2;
    if (instance.transform_at_end.has_value())
        instance_flags |= 1u << 4;
    words.push_back(blas_unit);
    words.push_back(instance.instance_id | (u32(instance.mask) << 24));
    words.push_back(instance.hit_group_offset | (instance_flags << 24));
    words.push_back(index);
    if (tlas_moves)
        for (auto k = 0; k < 12; ++k)
            words.push_back(float_bits(instance.transform_at_end.has_value() ? instance.transform_at_end.value()[k]
                                                                             : instance.transform[k]));
}

isize acceleration_position(cc::span<binding const> bindings, u32 binding_index)
{
    auto position = isize(0);
    for (auto const& b : bindings)
        if (b.type == binding_type::acceleration_structure && b.index < binding_index)
            ++position;
    return position;
}

isize acceleration_count(cc::span<binding const> bindings)
{
    auto count = isize(0);
    for (auto const& b : bindings)
        if (b.type == binding_type::acceleration_structure)
            ++count;
    return count;
}

bool traces_in_software(cc::span<binding const> bindings)
{
    for (auto const& b : bindings)
        if (b.traced_in_software)
            return true;
    return false;
}

// -- the pool --

void software_acceleration_pool::grow(state& s, isize min_units)
{
    auto const old_units = s.storage.size_in_bytes / unit_bytes;
    // A free range reaching the end joins the new space, so only the rest is missing.
    auto tail = isize(0);
    if (!s.free.empty() && isize(s.free.back().unit) + isize(s.free.back().units) == old_units)
        tail = isize(s.free.back().units);
    // unit 0 is the empty TLAS, so an empty pool holds a unit less than it grows to
    auto const required = cc::max(old_units, isize(1)) + min_units - tail;
    if (required > max_capacity_units)
        throw allocation_exception("acceleration pool allocation failed", min_units * unit_bytes,
                                   cc::any_error(cc::format("the software acceleration pool would outgrow {} bytes",
                                                            max_capacity_units * unit_bytes)));
    auto const new_units = cc::min(cc::max(cc::max(old_units * 2, required), initial_capacity_units), max_capacity_units);

    auto grown = create_storage(new_units * unit_bytes);
    if (grown.has_error())
        throw allocation_exception("acceleration pool allocation failed", new_units * unit_bytes,
                                   cc::move(grown.error()));
    auto const storage = grown.value();

    auto first_new = u32(old_units);
    if (old_units > 0)
    {
        cc::memcpy(storage.mapped, s.storage.mapped, size_t(s.storage.size_in_bytes));
        retire_storage(s.storage);
    }
    else
        first_new = 1; // unit 0 is the empty TLAS, whose zeroed header the storage already holds

    if (tail > 0)
        s.free.back().units += u32(new_units - old_units);
    else
        s.free.push_back({.unit = first_new, .units = u32(new_units - first_new)});
    s.storage = storage;
}

u32 software_acceleration_pool::allocate(isize units)
{
    CC_ASSERT(units > 0, "an acceleration region holds at least its header");
    return _state.lock(
        [&](state& s)
        {
            for (auto attempt = 0; attempt < 2; ++attempt)
            {
                for (isize i = 0; i < s.free.size(); ++i)
                {
                    auto& range = s.free[i];
                    if (isize(range.units) < units)
                        continue;
                    auto const unit = range.unit;
                    range.unit += u32(units);
                    range.units -= u32(units);
                    if (range.units == 0)
                        s.free.remove_at(i);
                    return unit;
                }
                grow(s, units);
            }
            CC_UNREACHABLE("a grown acceleration pool holds the region it grew for");
        });
}

void software_acceleration_pool::write(u32 unit, cc::span<u32 const> words)
{
    _state.lock([&](state& s)
                { cc::memcpy(s.storage.mapped + isize(unit) * unit_bytes, words.data(), size_t(words.size() * 4)); });
}

void software_acceleration_pool::release(u32 unit, u32 units)
{
    _ctx.defer_until_retired(
        [this, unit, units]
        {
            _state.lock(
                [&](state& s)
                {
                    // sorted and coalesced, so a list of many small regions does not fragment the pool
                    auto at = isize(0);
                    while (at < s.free.size() && s.free[at].unit < unit)
                        ++at;
                    s.free.insert_at(at, {.unit = unit, .units = units});
                    if (at + 1 < s.free.size() && s.free[at].unit + s.free[at].units == s.free[at + 1].unit)
                    {
                        s.free[at].units += s.free[at + 1].units;
                        s.free.remove_at(at + 1);
                    }
                    if (at > 0 && s.free[at - 1].unit + s.free[at - 1].units == s.free[at].unit)
                    {
                        s.free[at - 1].units += s.free[at].units;
                        s.free.remove_at(at);
                    }
                });
        });
}

software_pool_storage software_acceleration_pool::storage()
{
    return _state.lock(
        [&](state& s)
        {
            if (s.storage.mapped == nullptr)
                grow(s, 1);
            return s.storage;
        });
}

// -- builds --

software_region build_software_blas(software_acceleration_pool& pool, cc::span<blas_triangles const> geometries)
{
    auto triangle_count = u32(0);
    auto is_moving = false;
    for (auto const& g : geometries)
    {
        CC_ASSERT(!g.cpu_vertices.empty(), "build_blas: accel_build_flag::software_trace builds on the CPU, so a "
                                           "geometry needs cpu_vertices: sg reads no GPU buffer back for a build");
        CC_ASSERT(g.indices == nullptr || !g.cpu_indices.empty(), "build_blas: accel_build_flag::software_trace needs "
                                                                  "cpu_indices beside indices");
        CC_ASSERT(g.transform == nullptr || !g.cpu_transform.empty(), "build_blas: accel_build_flag::software_trace "
                                                                      "needs cpu_transform beside transform");
        CC_ASSERT(g.micromap == nullptr, "build_blas: the software walk has no opacity micromaps");
        for (auto const& pose : g.later_poses)
        {
            CC_ASSERT(!pose.cpu_vertices.empty(), "build_blas: accel_build_flag::software_trace needs a later pose's "
                                                  "cpu_vertices");
            is_moving = true;
        }
        triangle_count += u32(g.indices != nullptr ? g.index_count / 3 : g.vertex_count / 3);
    }

    auto const tree = lay_out_acceleration_tree(triangle_count);
    auto const records = 1 + 2 * isize(tree.node_count);
    auto const stride = is_moving ? isize(6) : isize(3);
    auto const units = records + stride * isize(triangle_count);
    auto const unit = pool.allocate(units);
    auto words = region_words(tree, units, unit + u32(records), triangle_count, is_moving ? 4u : 0u);

    auto first_triangle = isize(0);
    for (isize geometry = 0; geometry < geometries.size(); ++geometry)
    {
        auto const& g = geometries[geometry];
        auto const count = g.indices != nullptr ? g.index_count / 3 : g.vertex_count / 3;

        auto const index_at = [&](isize k) -> isize
        {
            if (g.indices == nullptr)
                return k;
            auto const wide = g.index_type != index_format::uint16;
            auto const at = g.index_offset_in_bytes + k * (wide ? 4 : 2);
            CC_ASSERT(at + (wide ? 4 : 2) <= g.cpu_indices.size(), "build_blas: cpu_indices is shorter than the "
                                                                   "indices");
            auto value = u32(0);
            cc::memcpy(&value, g.cpu_indices.data() + at, wide ? 4 : 2);
            CC_ASSERT(isize(value) < g.vertex_count, "build_blas: an index is past vertex_count");
            return isize(value);
        };
        auto const place = [&](float3 const& p) -> float3
        {
            if (g.transform == nullptr)
                return p;
            CC_ASSERT(g.transform_offset_in_bytes + 48 <= g.cpu_transform.size(), "build_blas: cpu_transform is too "
                                                                                  "short");
            float m[12];
            cc::memcpy(m, g.cpu_transform.data() + g.transform_offset_in_bytes, 48);
            return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                    m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
        };
        auto const write_vertex = [&](isize unit_at, float3 const& p, u32 w)
        {
            auto const at = unit_at * words_per_unit;
            words[at] = float_bits(p.x);
            words[at + 1] = float_bits(p.y);
            words[at + 2] = float_bits(p.z);
            words[at + 3] = w;
        };

        for (isize i = 0; i < count; ++i)
        {
            auto const at = records + stride * (first_triangle + i);
            u32 const tags[3] = {u32(i), u32(geometry), g.is_opaque ? 1u : 0u};
            for (auto v = 0; v < 3; ++v)
            {
                auto const offset = index_at(3 * i + v) * g.vertex_stride_in_bytes;
                write_vertex(at + v,
                             place(read_float3(g.cpu_vertices, g.vertex_offset_in_bytes + offset, "cpu_vertices")),
                             tags[v]);
                if (!is_moving)
                    continue;
                // a still geometry of a moving BLAS ends where it starts
                auto const end = g.later_poses.empty()
                                   ? read_float3(g.cpu_vertices, g.vertex_offset_in_bytes + offset, "cpu_vertices")
                                   : read_float3(g.later_poses[0].cpu_vertices, g.later_poses[0].offset_in_bytes + offset,
                                                 "a later pose's cpu_vertices");
                write_vertex(at + 3 + v, place(end), 0);
            }
        }
        first_triangle += count;
    }

    auto const vertex = [&](isize at, int v)
    {
        auto const first = (records + stride * at + v) * words_per_unit;
        return float3{cc::bit_cast<float>(words[first]), cc::bit_cast<float>(words[first + 1]),
                      cc::bit_cast<float>(words[first + 2])};
    };
    auto const box = fill_boxes(words, tree,
                                [&](u32 j)
                                {
                                    auto out = bounds();
                                    for (auto v = 0; v < int(stride); ++v)
                                        out.add(vertex(j, v));
                                    if (!is_moving)
                                        return out;
                                    // a vertex moves on a line between its two poses, widened since the mix rounds
                                    for (auto i = 0; i < 3; ++i)
                                    {
                                        auto const pad = 1.0e-6f * cc::max(magnitude(out.lo[i]), magnitude(out.hi[i]));
                                        out.lo[i] -= pad;
                                        out.hi[i] += pad;
                                    }
                                    return out;
                                });
    return written_region(pool, unit, units, words, box, is_moving);
}

software_region build_software_blas(software_acceleration_pool& pool, cc::span<blas_aabbs const> geometries)
{
    auto box_count = u32(0);
    for (auto const& g : geometries)
    {
        CC_ASSERT(!g.cpu_aabbs.empty(), "build_blas: accel_build_flag::software_trace builds on the CPU, so a geometry "
                                        "needs cpu_aabbs: sg reads no GPU buffer back for a build");
        box_count += u32(g.aabb_count);
    }

    auto const tree = lay_out_acceleration_tree(box_count);
    auto const records = 1 + 2 * isize(tree.node_count);
    auto const units = records + 2 * isize(box_count);
    auto const unit = pool.allocate(units);
    auto words = region_words(tree, units, unit + u32(records), box_count, 1u);

    auto boxes = cc::vector<bounds>();
    for (isize geometry = 0; geometry < geometries.size(); ++geometry)
    {
        auto const& g = geometries[geometry];
        for (isize i = 0; i < g.aabb_count; ++i)
        {
            auto const source = g.aabb_offset_in_bytes + i * g.aabb_stride_in_bytes;
            auto const lo = read_float3(g.cpu_aabbs, source, "cpu_aabbs");
            auto const hi = read_float3(g.cpu_aabbs, source + 12, "cpu_aabbs");
            auto const at = (records + 2 * boxes.size()) * words_per_unit;
            for (auto c = 0; c < 3; ++c)
            {
                words[at + c] = float_bits(lo[c]);
                words[at + words_per_unit + c] = float_bits(hi[c]);
            }
            words[at + 3] = u32(i);
            words[at + words_per_unit + 3] = u32(geometry) | (g.is_opaque ? 0x80000000u : 0u);
            auto box = bounds();
            box.add(lo);
            box.add(hi);
            boxes.push_back(box);
        }
    }
    auto const box = fill_boxes(words, tree, [&](u32 j) { return boxes[j]; });
    return written_region(pool, unit, units, words, box, false);
}

software_region build_software_tlas(software_acceleration_pool& pool, cc::span<tlas_instance const> instances)
{
    auto kinds_present = u32(0);
    auto instances_move = false;
    auto reaches_motion = false;
    for (auto const& instance : instances)
    {
        auto const& region = software_region_of(*instance.blas);
        CC_ASSERT(region.pool == &pool,
                  "build_tlas: accel_build_flag::software_trace needs every instance's blas built "
                  "with it too");
        kinds_present |= instance.blas->geometry() == blas_geometry::aabbs ? 2u : 1u;
        instances_move = instances_move || instance.transform_at_end.has_value();
        reaches_motion = reaches_motion || instance.transform_at_end.has_value() || region.is_moving;
    }

    auto const instance_count = u32(instances.size());
    auto const tree = lay_out_acceleration_tree(instance_count);
    auto const records = 1 + 2 * isize(tree.node_count);
    auto const stride = instances_move ? isize(10) : isize(7);
    auto const units = records + stride * isize(instance_count);
    auto const unit = pool.allocate(units);

    auto instance_words = cc::vector<u32>();
    instance_words.reserve(words_per_unit * stride * isize(instance_count));
    for (auto i = u32(0); i < instance_count; ++i)
        append_instance_words(instance_words, instances[i], software_region_of(*instances[i].blas).unit, i,
                              instances_move);

    auto words
        = region_words(tree, units, unit + u32(records), instance_count, kinds_present | (instances_move ? 4u : 0u));
    for (isize i = 0; i < instance_words.size(); ++i)
        words[records * words_per_unit + i] = instance_words[i];

    auto const box = fill_boxes(words, tree,
                                [&](u32 j)
                                {
                                    auto const& instance = instances[j];
                                    auto const& region = software_region_of(*instance.blas);
                                    auto blas_box = bounds();
                                    for (auto i = 0; i < 3; ++i)
                                    {
                                        blas_box.lo[i] = region.box_min[i];
                                        blas_box.hi[i] = region.box_max[i];
                                    }
                                    auto out = moved_box(blas_box, instance.transform);
                                    // a point moves on a line between its two places, so both boxes hold every time
                                    if (instance.transform_at_end.has_value())
                                        out.add(moved_box(blas_box, instance.transform_at_end.value().data()));
                                    return out;
                                });
    return written_region(pool, unit, units, words, box, reaches_motion);
}
} // namespace sg::impl
