// webgpu_acceleration_pool: the storage every acceleration structure lives in, and the kernels that build into it.
// The layout both follow is libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md.

#include <clean-core/common/assert.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/webgpu/webgpu_acceleration.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg::backend::webgpu
{
namespace
{
/// The first pool, 64 KiB.
constexpr isize initial_capacity_units = 4096;

/// The pool is bound whole, so it can grow no larger than the device's maxStorageBufferBindingSize.
/// The device is requested with the default limits, which is exactly this.
constexpr isize max_capacity_units = isize(128) * 1024 * 1024 / acceleration_unit_bytes;

// Every kernel takes one item per invocation, `a.x` being the first item of this dispatch and `a.y` how many there are.
// The rest of `args` is per kernel, as each one's comment says.
constexpr char const* k_build_kernels = R"(
struct build_args {
    a: vec4u,
    b: vec4u,
    c: vec4u,
    d: vec4u,
}

struct bounds {
    lo: vec3f,
    hi: vec3f,
}

@group(0) @binding(0) var<uniform> args: build_args;
@group(0) @binding(1) var<storage, read_write> pool: array<vec4u>;
@group(0) @binding(2) var<storage, read> input0: array<u32>;
@group(0) @binding(3) var<storage, read> input1: array<u32>;
@group(0) @binding(4) var<storage, read> input2: array<u32>;
// The spatial sort's working words; atomic only because the bounds reduce into it, every other access a plain load or store.
@group(0) @binding(5) var<storage, read_write> scratch: array<atomic<u32>>;

const huge: f32 = 3.0e38;

fn words3(first: u32) -> vec3u {
    return vec3u(input0[first], input0[first + 1u], input0[first + 2u]);
}

fn as_vec3f(v: vec4u) -> vec3f {
    return bitcast<vec3f>(v.xyz);
}

fn node_unit(node: u32) -> u32 {
    return args.a.z + 1u + 2u * node;
}

// write_triangles
//   a: z the geometry's first record unit, w the geometry index
//   b: x vertex offset in words, y vertex stride in words, z indices (0 none, 1 u16, 2 u32), w index offset in bytes
//   c: x 1 with a transform, y transform offset in words, z 1 when opaque, w units a triangle takes
fn index_at(k: u32) -> u32 {
    if (args.b.z == 1u) {
        let byte_at = args.b.w + 2u * k;
        return (input1[byte_at / 4u] >> ((byte_at & 2u) * 8u)) & 0xffffu;
    }
    if (args.b.z == 2u) {
        return input1[args.b.w / 4u + k];
    }
    return k;
}

fn transformed_row(row: u32, p: vec3f) -> f32 {
    let first = args.c.y + 4u * row;
    let m = bitcast<vec4f>(vec4u(input2[first], input2[first + 1u], input2[first + 2u], input2[first + 3u]));
    return dot(m.xyz, p) + m.w;
}

fn vertex_bits(k: u32) -> vec3u {
    let bits = words3(args.b.x + index_at(k) * args.b.y);
    if (args.c.x == 0u) {
        return bits;
    }
    let p = bitcast<vec3f>(bits);
    return bitcast<vec3u>(vec3f(transformed_row(0u, p), transformed_row(1u, p), transformed_row(2u, p)));
}

@compute @workgroup_size(64)
fn write_triangles(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let i = args.a.x + id.x;
    let first = args.a.z + args.c.w * i;
    pool[first] = vec4u(vertex_bits(3u * i), i);
    pool[first + 1u] = vec4u(vertex_bits(3u * i + 1u), args.a.w);
    pool[first + 2u] = vec4u(vertex_bits(3u * i + 2u), args.c.z);
}

// write_triangle_ends: units 3 to 5 of a moving BLAS's 6-unit triangle, from the pose at time 1 in input0.
// Its args are write_triangles', the indices and the transform shared with the start pose.
@compute @workgroup_size(64)
fn write_triangle_ends(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let i = args.a.x + id.x;
    let first = args.a.z + args.c.w * i;
    pool[first + 3u] = vec4u(vertex_bits(3u * i), 0u);
    pool[first + 4u] = vec4u(vertex_bits(3u * i + 1u), 0u);
    pool[first + 5u] = vec4u(vertex_bits(3u * i + 2u), 0u);
}

// write_boxes
//   a: z the geometry's first record unit, w the geometry index
//   b: x box offset in words, y box stride in words
//   c: z 1 when opaque
@compute @workgroup_size(64)
fn write_boxes(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let i = args.a.x + id.x;
    let source = args.b.x + i * args.b.y;
    let first = args.a.z + 2u * i;
    pool[first] = vec4u(words3(source), i);
    pool[first + 1u] = vec4u(words3(source + 3u), args.a.w | (args.c.z << 31u));
}

// write_leaves
//   a: z the region's unit, w what a leaf holds (0 triangles, 1 boxes, 2 instances, 3 instances that move, 4 triangles
//      that move)
//   b: x the first leaf's node index, y the first primitive or instance unit
fn primitive_bounds(j: u32) -> bounds {
    if (args.a.w == 0u) {
        let first = args.b.y + 3u * j;
        let v0 = as_vec3f(pool[first]);
        let v1 = as_vec3f(pool[first + 1u]);
        let v2 = as_vec3f(pool[first + 2u]);
        return bounds(min(v0, min(v1, v2)), max(v0, max(v1, v2)));
    }
    if (args.a.w == 4u) {
        // Kind 4 is triangles that move, 6 units apiece: a vertex moves on a line between its two poses, so the box of
        // both holds it at every time, widened since the mix rounds.
        let first = args.b.y + 6u * j;
        let v0 = as_vec3f(pool[first]);
        let v1 = as_vec3f(pool[first + 1u]);
        let v2 = as_vec3f(pool[first + 2u]);
        let e0 = as_vec3f(pool[first + 3u]);
        let e1 = as_vec3f(pool[first + 4u]);
        let e2 = as_vec3f(pool[first + 5u]);
        let lo = min(min(v0, min(v1, v2)), min(e0, min(e1, e2)));
        let hi = max(max(v0, max(v1, v2)), max(e0, max(e1, e2)));
        let pad = 1.0e-6 * max(abs(lo), abs(hi));
        return bounds(lo - pad, hi + pad);
    }
    if (args.a.w == 1u) {
        let first = args.b.y + 2u * j;
        return bounds(as_vec3f(pool[first]), as_vec3f(pool[first + 1u]));
    }

    // An instance: its BLAS root box, the node right after that region's header, moved to world space.
    // Kind 3 is instances that move, 10 units apiece: a point moves on a line between its two places, so the box of
    // both transforms holds it at every time.
    let stride = select(7u, 10u, args.a.w == 3u);
    let first = args.b.y + stride * j;
    let start = instance_box(first, first + 3u);
    if (args.a.w != 3u) {
        return start;
    }
    let end = instance_box(first, first + 7u);
    return bounds(min(start.lo, end.lo), max(start.hi, end.hi));
}

/// The instance at `first`'s BLAS root box under the transform whose rows start at `rows`.
fn instance_box(first: u32, rows: u32) -> bounds {
    let blas = pool[first + 6u].x;
    let lo = as_vec3f(pool[blas + 1u]);
    let hi = as_vec3f(pool[blas + 2u]);
    let center = 0.5 * (lo + hi);
    let extent = 0.5 * (hi - lo);
    var world_center = vec3f(0.0);
    var world_extent = vec3f(0.0);
    var pad = vec3f(0.0);
    for (var row = 0u; row < 3u; row++) {
        let m = bitcast<vec4f>(pool[rows + row]);
        world_center[row] = dot(m.xyz, center) + m.w;
        world_extent[row] = dot(abs(m.xyz), extent);
        // The transform rounds, and the box must still hold every point of the instance.
        // The rounding scales with the terms summed rather than with the sum, which cancellation can make small.
        pad[row] = 1.0e-6 * (dot(abs(m.xyz), abs(center)) + abs(m.w) + world_extent[row]);
    }
    return bounds(world_center - world_extent - pad, world_center + world_extent + pad);
}

@compute @workgroup_size(64)
fn write_leaves(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let u = node_unit(args.b.x + args.a.x + id.x);
    let low = pool[u];
    let high = pool[u + 1u];
    var acc = bounds(vec3f(huge), vec3f(-huge));
    let count = high.w & 0x7fffffffu;
    for (var k = 0u; k < count; k++) {
        let p = primitive_bounds(low.w + k);
        acc.lo = min(acc.lo, p.lo);
        acc.hi = max(acc.hi, p.hi);
    }
    pool[u] = vec4u(bitcast<vec3u>(acc.lo), low.w);
    pool[u + 1u] = vec4u(bitcast<vec3u>(acc.hi), high.w);
}

// write_levels
//   a: z the region's unit
//   b: x the level's first node index
@compute @workgroup_size(64)
fn write_levels(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let u = node_unit(args.b.x + args.a.x + id.x);
    let low = pool[u];
    let high = pool[u + 1u];
    let left = node_unit(low.w);
    let right = node_unit(high.w);
    let lo = min(as_vec3f(pool[left]), as_vec3f(pool[right]));
    let hi = max(as_vec3f(pool[left + 1u]), as_vec3f(pool[right + 1u]));
    pool[u] = vec4u(bitcast<vec3u>(lo), low.w);
    pool[u + 1u] = vec4u(bitcast<vec3u>(hi), high.w);
}

// ---- the spatial sort ----
//
// Reorders a region's records by the Morton code of their box centers before the tree is boxed, so a leaf and every
// node above it hold primitives that lie together. A record carries its own primitive, geometry or instance index,
// so a hit reports the caller's index whatever position the sort gave the record.
// Scratch words 0 to 2 hold the centers' minimum as ordered bits, 3 to 5 their maximum; the rest are placed by the host.
// Every kernel reads its records as write_leaves does: a.w what they are, b.y the first record's unit.

// The bits of `f`, flipped so that unsigned order is float order.
fn ordered_bits(f: f32) -> u32 {
    let b = bitcast<u32>(f);
    if ((b & 0x80000000u) != 0u) {
        return ~b;
    }
    return b | 0x80000000u;
}

fn from_ordered_bits(o: u32) -> f32 {
    if ((o & 0x80000000u) != 0u) {
        return bitcast<f32>(o & 0x7fffffffu);
    }
    return bitcast<f32>(~o);
}

fn center_of(j: u32) -> vec3f {
    let b = primitive_bounds(j);
    return 0.5 * (b.lo + b.hi);
}

// The low 10 bits of `v`, moved to every third bit.
fn spread_bits(v: u32) -> u32 {
    var x = v & 0x3ffu;
    x = (x | (x << 16u)) & 0x030000ffu;
    x = (x | (x << 8u)) & 0x0300f00fu;
    x = (x | (x << 4u)) & 0x030c30c3u;
    x = (x | (x << 2u)) & 0x09249249u;
    return x;
}

// sort_clear
//   b: z the first word, w the value every word is set to
@compute @workgroup_size(64)
fn sort_clear(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    atomicStore(&scratch[args.b.z + args.a.x + id.x], args.b.w);
}

var<workgroup> group_lo: array<atomic<u32>, 3>;
var<workgroup> group_hi: array<atomic<u32>, 3>;

// sort_bounds: the centers' box, reduced in each workgroup before one atomic per group reaches the scratch.
@compute @workgroup_size(64)
fn sort_bounds(@builtin(global_invocation_id) id: vec3u, @builtin(local_invocation_index) local: u32) {
    if (local < 3u) {
        atomicStore(&group_lo[local], 0xffffffffu);
        atomicStore(&group_hi[local], 0u);
    }
    workgroupBarrier();
    if (id.x < args.a.y) {
        let c = center_of(args.a.x + id.x);
        for (var k = 0u; k < 3u; k++) {
            atomicMin(&group_lo[k], ordered_bits(c[k]));
            atomicMax(&group_hi[k], ordered_bits(c[k]));
        }
    }
    workgroupBarrier();
    if (local < 3u) {
        atomicMin(&scratch[local], atomicLoad(&group_lo[local]));
        atomicMax(&scratch[3u + local], atomicLoad(&group_hi[local]));
    }
}

// sort_keys
//   c: x the keys' first word, y the values' first word
@compute @workgroup_size(64)
fn sort_keys(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let j = args.a.x + id.x;
    let lo = vec3f(from_ordered_bits(atomicLoad(&scratch[0])), from_ordered_bits(atomicLoad(&scratch[1])),
                   from_ordered_bits(atomicLoad(&scratch[2])));
    let hi = vec3f(from_ordered_bits(atomicLoad(&scratch[3])), from_ordered_bits(atomicLoad(&scratch[4])),
                   from_ordered_bits(atomicLoad(&scratch[5])));
    let f = clamp((center_of(j) - lo) / max(hi - lo, vec3f(1.0e-30)), vec3f(0.0), vec3f(1.0));
    let q = vec3u(f * 1023.0);
    atomicStore(&scratch[args.c.x + j], spread_bits(q.x) | (spread_bits(q.y) << 1u) | (spread_bits(q.z) << 2u));
    atomicStore(&scratch[args.c.y + j], j);
}

// One pass of a stable radix sort over 8 bits of the keys.
// The keys are cut into blocks, one invocation walking each in order, which is what keeps it stable with no subgroups.
// The counts are digit-major, a row of blocks per digit, so that each block's place is its row's base plus its prefix.
//   b: z the keys out, w the values out
//   c: x the keys in, y the values in, z the counts, w the row bases
//   d: x the record count, y the block count, z the shift, w the records per block
fn block_digit(i: u32) -> u32 {
    return (atomicLoad(&scratch[args.c.x + i]) >> args.d.z) & 255u;
}

@compute @workgroup_size(64)
fn sort_count(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let block = args.a.x + id.x;
    let end = min((block + 1u) * args.d.w, args.d.x);
    for (var i = block * args.d.w; i < end; i++) {
        let at = args.c.z + block_digit(i) * args.d.y + block;
        atomicStore(&scratch[at], atomicLoad(&scratch[at]) + 1u);
    }
}

// sort_scan_rows: one invocation per digit turns its row of counts into an exclusive prefix, and leaves the row's total as its base.
@compute @workgroup_size(64)
fn sort_scan_rows(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let digit = args.a.x + id.x;
    var running = 0u;
    for (var block = 0u; block < args.d.y; block++) {
        let at = args.c.z + digit * args.d.y + block;
        let count = atomicLoad(&scratch[at]);
        atomicStore(&scratch[at], running);
        running += count;
    }
    atomicStore(&scratch[args.c.w + digit], running);
}

// sort_scan_bases: one invocation turns the 256 row totals into each row's first place.
@compute @workgroup_size(64)
fn sort_scan_bases(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    var running = 0u;
    for (var digit = 0u; digit < 256u; digit++) {
        let at = args.c.w + digit;
        let total = atomicLoad(&scratch[at]);
        atomicStore(&scratch[at], running);
        running += total;
    }
}

@compute @workgroup_size(64)
fn sort_scatter(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let block = args.a.x + id.x;
    let end = min((block + 1u) * args.d.w, args.d.x);
    for (var i = block * args.d.w; i < end; i++) {
        let digit = block_digit(i);
        let at = args.c.z + digit * args.d.y + block;
        let offset = atomicLoad(&scratch[at]);
        atomicStore(&scratch[at], offset + 1u);
        let place = atomicLoad(&scratch[args.c.w + digit]) + offset;
        atomicStore(&scratch[args.b.z + place], atomicLoad(&scratch[args.c.x + i]));
        atomicStore(&scratch[args.b.w + place], atomicLoad(&scratch[args.c.y + i]));
    }
}

// sort_gather: the records in sorted order, into the scratch, which the host then copies over the region's own.
//   b: x units per record, y the first record's unit, z the first scratch word written
//   c: y the sorted values
@compute @workgroup_size(64)
fn sort_gather(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= args.a.y) {
        return;
    }
    let i = args.a.x + id.x;
    let source = args.b.y + args.b.x * atomicLoad(&scratch[args.c.y + i]);
    for (var k = 0u; k < args.b.x; k++) {
        let v = pool[source + k];
        let to = args.b.z + 4u * (args.b.x * i + k);
        atomicStore(&scratch[to], v.x);
        atomicStore(&scratch[to + 1u], v.y);
        atomicStore(&scratch[to + 2u], v.z);
        atomicStore(&scratch[to + 3u], v.w);
    }
}
)";

constexpr char const* k_kernel_entry_points[] = {
    "write_triangles", "write_triangle_ends", "write_boxes", "write_leaves", "write_levels",
    "sort_clear",      "sort_bounds",         "sort_keys",   "sort_count",   "sort_scan_rows",
    "sort_scan_bases", "sort_scatter",        "sort_gather",
};

[[nodiscard]] wgpu_buffer create_pool_buffer(WGPUDevice device, isize units, char const* label)
{
    auto const desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu(label),
        .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst,
        .size = u64(units * acceleration_unit_bytes),
        .mappedAtCreation = WGPU_FALSE,
    };
    return wgpu_buffer(wgpuDeviceCreateBuffer(device, &desc));
}
} // namespace

void webgpu_acceleration_pool::initialize(webgpu_context& ctx)
{
    _ctx = &ctx;
}

void webgpu_acceleration_pool::shutdown()
{
    _shut_down = true;
    _kernels.clear();
    _kernel_module = {};
    _kernel_pipeline_layout = {};
    _kernel_layout = {};
    _unused_input = {};
    _sort_scratch = {};
    _sort_scratch_words = 0;
    _buffer = {};
    _free.clear();
}

wgpu_buffer const& webgpu_acceleration_pool::buffer()
{
    CC_ASSERT(!_shut_down, "the acceleration pool is used after its context shut down");
    if (!_buffer)
    {
        _buffer = create_pool_buffer(_ctx->device(), initial_capacity_units, "sg acceleration pool");
        CC_ASSERT(bool(_buffer), "wgpuDeviceCreateBuffer returned no acceleration pool");
        _capacity_units = initial_capacity_units;
        _generation = 1;
        // Unit 0 is the empty TLAS, and zero-filled is exactly its header.
        _free.push_back({.unit = 1, .units = u32(_capacity_units - 1)});
    }
    return _buffer;
}

u32 webgpu_acceleration_pool::allocate(isize units)
{
    CC_ASSERT(units > 0, "an acceleration region holds at least its header");
    (void)buffer();

    for (auto attempt = 0; attempt < 2; ++attempt)
    {
        for (isize i = 0; i < _free.size(); ++i)
        {
            auto& range = _free[i];
            if (isize(range.units) < units)
                continue;
            auto const unit = range.unit;
            range.unit += u32(units);
            range.units -= u32(units);
            if (range.units == 0)
                _free.remove_at(i);
            return unit;
        }
        grow(units);
    }
    CC_UNREACHABLE("a grown acceleration pool holds the region it grew for");
}

void webgpu_acceleration_pool::grow(isize min_units)
{
    // A free range reaching the end joins the new space, so only the rest is missing.
    auto tail = isize(0);
    if (!_free.empty() && isize(_free.back().unit) + isize(_free.back().units) == _capacity_units)
        tail = isize(_free.back().units);
    auto const required = _capacity_units + min_units - tail;
    if (required > max_capacity_units)
        throw sg::allocation_exception("acceleration pool allocation failed", min_units * acceleration_unit_bytes,
                                       cc::any_error(cc::format("the webgpu acceleration pool would outgrow the "
                                                                "largest storage binding, {} bytes",
                                                                max_capacity_units * acceleration_unit_bytes)));
    auto const new_units = cc::min(cc::max(_capacity_units * 2, required), max_capacity_units);

    auto grown = create_pool_buffer(_ctx->device(), new_units, "sg acceleration pool");
    CC_ASSERT(bool(grown), "wgpuDeviceCreateBuffer returned no acceleration pool");

    // In a submit of its own, so it orders after every list submitted so far and before every list submitted later.
    // A list still open that wrote into the old buffer copies its own regions again (bring_pool_writes_forward).
    auto const encoder_desc = WGPUCommandEncoderDescriptor{.nextInChain = nullptr, .label = to_wgpu("sg pool growth")};
    auto encoder = wgpu_command_encoder(wgpuDeviceCreateCommandEncoder(_ctx->device(), &encoder_desc));
    wgpuCommandEncoderCopyBufferToBuffer(encoder.get(), _buffer.get(), 0, grown.get(), 0,
                                         u64(_capacity_units * acceleration_unit_bytes));
    auto const buffer_desc = WGPUCommandBufferDescriptor{.nextInChain = nullptr, .label = to_wgpu("sg pool growth")};
    auto const commands = wgpu_command_buffer(wgpuCommandEncoderFinish(encoder.get(), &buffer_desc));
    auto const raw = commands.get();
    wgpuQueueSubmit(_ctx->queue(), 1, &raw);

    // An open list may still submit work naming the old buffer, and every one of them submits before this epoch retires.
    auto expiring = webgpu_expiring_resource{};
    expiring.buffer = cc::move(_buffer);
    _ctx->schedule_deferred_deletion(cc::move(expiring));

    if (tail > 0)
        _free.back().units += u32(new_units - _capacity_units);
    else
        _free.push_back({.unit = u32(_capacity_units), .units = u32(new_units - _capacity_units)});
    _buffer = cc::move(grown);
    _capacity_units = new_units;
    ++_generation;
}

void webgpu_acceleration_pool::release_when_retired(u32 unit, u32 units, cc::vector<cc::unique_function<void()>> finalizers)
{
    auto expiring = webgpu_expiring_resource{};
    expiring.finalizers.push_back([this, unit, units] { free_now(unit, units); });
    for (auto& f : finalizers)
        expiring.finalizers.push_back(cc::move(f));
    _ctx->schedule_deferred_deletion(cc::move(expiring));
}

void webgpu_acceleration_pool::free_now(u32 unit, u32 units)
{
    if (_shut_down || units == 0)
        return;

    auto at = isize(0);
    while (at < _free.size() && _free[at].unit < unit)
        ++at;
    CC_ASSERT(at == _free.size() || unit + units <= _free[at].unit, "an acceleration region is released twice");
    CC_ASSERT(at == 0 || _free[at - 1].unit + _free[at - 1].units <= unit, "an acceleration region is released twice");

    auto const joins_previous = at > 0 && _free[at - 1].unit + _free[at - 1].units == unit;
    auto const joins_next = at < _free.size() && unit + units == _free[at].unit;
    if (joins_previous && joins_next)
    {
        _free[at - 1].units += units + _free[at].units;
        _free.remove_at(at);
    }
    else if (joins_previous)
        _free[at - 1].units += units;
    else if (joins_next)
    {
        _free[at].unit = unit;
        _free[at].units += units;
    }
    else
        _free.insert_at(at, free_range{.unit = unit, .units = units});
}

void webgpu_acceleration_pool::create_kernels()
{
    auto const device = _ctx->device();

    auto entries = cc::vector<WGPUBindGroupLayoutEntry>();
    for (auto binding = u32(0); binding < 6; ++binding)
    {
        auto entry = WGPUBindGroupLayoutEntry{};
        entry.binding = binding;
        entry.visibility = WGPUShaderStage_Compute;
        entry.buffer.type = binding == 0                 ? WGPUBufferBindingType_Uniform
                          : binding == 1 || binding == 5 ? WGPUBufferBindingType_Storage
                                                         : WGPUBufferBindingType_ReadOnlyStorage;
        entries.push_back(entry);
    }
    auto const layout_desc = WGPUBindGroupLayoutDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg acceleration build"),
        .entryCount = size_t(entries.size()),
        .entries = entries.data(),
    };
    _kernel_layout = wgpu_bind_group_layout(wgpuDeviceCreateBindGroupLayout(device, &layout_desc));

    auto const set_layout = _kernel_layout.get();
    auto const pipeline_layout_desc = WGPUPipelineLayoutDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg acceleration build"),
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts = &set_layout,
        .immediateSize = 0,
    };
    _kernel_pipeline_layout = wgpu_pipeline_layout(wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_desc));

    auto source = WGPUShaderSourceWGSL{};
    source.chain.next = nullptr;
    source.chain.sType = WGPUSType_ShaderSourceWGSL;
    source.code = to_wgpu(k_build_kernels);
    auto const module_desc = WGPUShaderModuleDescriptor{
        .nextInChain = &source.chain,
        .label = to_wgpu("sg acceleration build"),
    };
    _kernel_module = wgpu_shader_module(wgpuDeviceCreateShaderModule(device, &module_desc));

    for (auto const* entry_point : k_kernel_entry_points)
    {
        auto desc = WGPUComputePipelineDescriptor{};
        desc.label = to_wgpu(entry_point);
        desc.layout = _kernel_pipeline_layout.get();
        desc.compute.module = _kernel_module.get();
        desc.compute.entryPoint = to_wgpu(entry_point);
        _kernels.push_back(wgpu_compute_pipeline(wgpuDeviceCreateComputePipeline(device, &desc)));
        CC_ASSERT(bool(_kernels.back()), "wgpuDeviceCreateComputePipeline returned no acceleration build kernel");
    }

    auto const input_desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg acceleration unused input"),
        .usage = WGPUBufferUsage_Storage,
        .size = u64(acceleration_unit_bytes),
        .mappedAtCreation = WGPU_FALSE,
    };
    _unused_input = wgpu_buffer(wgpuDeviceCreateBuffer(device, &input_desc));
}

wgpu_buffer const& webgpu_acceleration_pool::sort_scratch(isize min_words)
{
    CC_ASSERT(!_shut_down, "the acceleration pool is used after its context shut down");
    CC_ASSERT(min_words * 4 <= max_sort_scratch_bytes, "the sort scratch is bound whole, so it fits one storage "
                                                       "binding");
    if (!_sort_scratch || _sort_scratch_words < min_words)
    {
        // At least a little, so every build kernel has something to bind, and doubling, so a run of growing builds
        // reallocates a few times rather than once each.
        auto const words
            = cc::min(cc::max(cc::max(min_words, isize(64)), 2 * _sort_scratch_words), max_sort_scratch_bytes / 4);
        auto const desc = WGPUBufferDescriptor{
            .nextInChain = nullptr,
            .label = to_wgpu("sg acceleration sort scratch"),
            .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc, // the sorted records are copied over the region
            .size = u64(words * 4),
            .mappedAtCreation = WGPU_FALSE,
        };
        _sort_scratch = wgpu_buffer(wgpuDeviceCreateBuffer(_ctx->device(), &desc));
        CC_ASSERT(bool(_sort_scratch), "wgpuDeviceCreateBuffer returned no sort scratch");
        _sort_scratch_words = words;
    }
    return _sort_scratch;
}

WGPUComputePipeline webgpu_acceleration_pool::kernel(acceleration_kernel k)
{
    CC_ASSERT(!_shut_down, "the acceleration pool is used after its context shut down");
    if (_kernels.empty())
        create_kernels();
    return _kernels[isize(k)].get();
}

WGPUBindGroupLayout webgpu_acceleration_pool::kernel_layout()
{
    if (_kernels.empty())
        create_kernels();
    return _kernel_layout.get();
}

WGPUBuffer webgpu_acceleration_pool::unused_input()
{
    if (_kernels.empty())
        create_kernels();
    return _unused_input.get();
}

// -- the structures --

webgpu_blas::webgpu_blas(webgpu_context& ctx,
                         u32 unit,
                         u32 units,
                         primitive_kind kind,
                         sg::accel_build_flags build_flags,
                         int geometry_count)
  : sg::blas(isize(units) * acceleration_unit_bytes, 0, 0, build_flags, geometry_count),
    _ctx(ctx),
    _unit(unit),
    _units(units),
    _kind(kind)
{
}

webgpu_blas::~webgpu_blas()
{
    release_region();
}

void webgpu_blas::release_region() const
{
    if (_released)
        return;
    _released = true;
    auto finalizers = cc::move(_finalizers);
    _finalizers.clear();
    _ctx._acceleration.release_when_retired(_unit, _units, cc::move(finalizers));
}

webgpu_tlas::webgpu_tlas(webgpu_context& ctx,
                         u32 unit,
                         u32 units,
                         sg::accel_build_flags build_flags,
                         int instance_count,
                         cc::vector<sg::blas_handle> referenced_blases)
  : sg::tlas(isize(units) * acceleration_unit_bytes, 0, 0, build_flags, instance_count, cc::move(referenced_blases)),
    _ctx(ctx),
    _unit(unit),
    _units(units)
{
}

webgpu_tlas::~webgpu_tlas()
{
    release_region();
}

void webgpu_tlas::release_region() const
{
    if (_released)
        return;
    _released = true;
    auto finalizers = cc::move(_finalizers);
    _finalizers.clear();
    _ctx._acceleration.release_when_retired(_unit, _units, cc::move(finalizers));
}
} // namespace sg::backend::webgpu
