#include "emit-test-support.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace cc::primitive_defines;

// EMIT-292: after a call point, the native routes compute again what reads only constants and the launch, rather than
// keep it across the call; slib's tests read the DXIL itself, through DXC.

namespace
{
constexpr cc::string_view k_source = R"(require raytracing_pipeline

binding gP:
    tint: float3
    scale: float
    mask: uint
    world: acceleration_structure[.triangles]
    data: mut buffer[float4]

struct payload:
    color: float3

struct operand:
    x: float

rays R:
    surface: payload

@callable fun twice(v: mut operand):
    v.x = v.x * 2.0

callables ops = (twice)

@raygen fun looped(@launch_id id: int3){gP}:
    let k = gP.tint * gP.scale
    let px = id.x * 3 + 1
    let before = gP.data[px]
    let mut c = float3(0.0)
    let mut i = 0u
    while i < 4u:
        let mut p = payload(float3(0.0))
        trace(gP.world, ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)), R.surface, mut p, mask = gP.mask as int)
        c += p.color * k
        i += 1u
    let after = gP.data[px]
    gP.data[px + id.y] = float4(c.x, c.y, c.z, 1.0) + before + after

@raygen fun called(@launch_id id: int3){gP}:
    let k = gP.scale * 2.0
    let mut v = operand(k)
    ops[0](mut v)
    gP.data[id.x] = float4(v.x * k, 0.0, 0.0, 1.0)

@raygen fun straight(@launch_id id: int3){gP}:
    let k = gP.scale * 2.0
    gP.data[id.x] = float4(k, 0.0, 0.0, 1.0)
    let mut p = payload(float3(k, k, k))
    trace(gP.world, ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)), R.surface, mut p)

@raygen fun reordered(@launch_id id: int3){gP}:
    let k = gP.scale * 2.0
    let mut p = payload(float3(0.0))
    const if has(.shader_execution_reordering):
        let h = trace_hit(gP.world, ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)), R.surface, mut p)
        let seen = k + 1.0
        h.invoke(mut p)
        gP.data[id.x] = float4(p.color.x * k, seen, 0.0, 1.0)

@miss fun sky(p: mut payload):
    p.color = float3(1.0)

hit_group hits for R:
    surface = ()

@raytracing pipeline path:
    rays = R
    raygen = looped
    miss.surface = sky
    hit_groups = (hits)
)";

[[nodiscard]] sgl::emitted_source native_of(cc::string_view entry, sgl::emit::target t, bool reorders = false)
{
    auto device = sgl::check::feature_set();
    if (reorders)
        device.set(sgl::check::feature::shader_execution_reordering);
    auto const e = sgl::compile_to_text({.source = k_source,
                                         .entry_point = entry,
                                         .target = t,
                                         .device_features = device,
                                         .native = sgl::emit::native_route::require});
    REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
    return e.value();
}

/// The lines of a program as `ir::dump` writes it, and what each operation's value is named.
struct program_lines
{
    cc::vector<cc::string> lines;

    explicit program_lines(cc::string_view text)
    {
        auto at = isize(0);
        while (at < text.size())
        {
            auto end = text.find('\n', at);
            if (end < 0)
                end = text.size();
            lines.push_back(cc::string(text.subview({.offset = at, .size = end - at})));
            at = end + 1;
        }
    }

    /// The first line from `from` on holding `part`, or -1.
    [[nodiscard]] isize find(cc::string_view part, isize from = 0) const
    {
        for (auto i = from; i < lines.size(); ++i)
            if (lines[i].contains(part))
                return i;
        return -1;
    }
    [[nodiscard]] isize count(cc::string_view part) const
    {
        auto n = isize(0);
        for (auto const& l : lines)
            n += l.contains(part) ? 1 : 0;
        return n;
    }
    /// The line defining `%name`, or an empty one.
    [[nodiscard]] cc::string_view definition(cc::string_view name) const
    {
        auto const head = cc::format("    {}: ", name);
        for (auto const& l : lines)
            if (l.starts_with(head))
                return l;
        return {};
    }
    /// The loads whose pointer is an access chain into `global`.
    [[nodiscard]] isize loads_from(cc::string_view global) const
    {
        auto n = isize(0);
        for (auto const& l : lines)
        {
            auto const at = l.find("= load %");
            if (at < 0)
                continue;
            auto const pointer = l.subview({.offset = at + 7, .size = l.size() - at - 7});
            n += definition(pointer).contains(cc::format("access_chain {} ", global)) ? 1 : 0;
        }
        return n;
    }
};

/// One SPIR-V instruction: its position, opcode and words.
struct spirv_walk
{
    cc::span<u32 const> words;

    [[nodiscard]] u32 opcode(isize at) const { return words[at] & 0xffffu; }
    [[nodiscard]] isize next(isize at) const { return at + isize(words[at] >> 16); }

    /// The id of each variable of storage class `storage`.
    [[nodiscard]] cc::vector<u32> variables(u32 storage) const
    {
        auto ids = cc::vector<u32>();
        for (auto at = isize(5); at < words.size(); at = next(at))
            if (opcode(at) == 59 && words[at + 3] == storage)
                ids.push_back(words[at + 2]);
        return ids;
    }
    /// The id of the variable decorated as built-in `which`.
    [[nodiscard]] u32 built_in(u32 which) const
    {
        for (auto at = isize(5); at < words.size(); at = next(at))
            if (opcode(at) == 71 && words[at + 2] == 11 && words[at + 3] == which)
                return words[at + 1];
        return 0;
    }
    /// The position of the first instruction of `code` from `from` on, or -1.
    [[nodiscard]] isize find(u32 code, isize from = 5) const
    {
        for (auto at = from; at < words.size(); at = next(at))
            if (opcode(at) == code)
                return at;
        return -1;
    }
    /// The positions of every access chain into one of `bases`.
    [[nodiscard]] cc::vector<isize> chains_into(cc::span<u32 const> bases) const
    {
        auto found = cc::vector<isize>();
        for (auto at = isize(5); at < words.size(); at = next(at))
            if (opcode(at) == 65)
                for (auto const b : bases)
                    if (words[at + 3] == b)
                        found.push_back(at);
        return found;
    }
    /// The loads of a pointer that is `variable` itself, or an access chain into one of `bases`.
    [[nodiscard]] isize loads_of(cc::span<u32 const> bases) const
    {
        auto chained = cc::vector<u32>();
        for (auto const at : chains_into(bases))
            chained.push_back(words[at + 2]);
        auto n = isize(0);
        for (auto at = isize(5); at < words.size(); at = next(at))
        {
            if (opcode(at) != 61)
                continue;
            for (auto const b : bases)
                n += words[at + 3] == b ? 1 : 0;
            for (auto const c : chained)
                n += words[at + 3] == c ? 1 : 0;
        }
        return n;
    }
};

constexpr u32 k_uniform = 2;
constexpr u32 k_storage_buffer = 12;
constexpr u32 k_ray_payload = 5338;
constexpr u32 k_launch_id = 5319;
constexpr u32 k_op_trace_ray = 4445;
constexpr u32 k_op_execute_callable = 4446;
constexpr u32 k_op_load = 61;
} // namespace

TEST("sgl emit - a constant block's value is read again after each trace it would be kept across (EMIT-292)")
{
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const e = native_of("looped", t);
        auto const program = program_lines(e.text);
        auto const trace = program.find("trace_ray ");
        REQUIRE(trace >= 0).dump("program", e.text);
        // `k`'s fields read after the trace, in the loop's body, and nowhere before it
        CHECK(program.find("access_chain @gP 0x0", trace) > trace).dump("program", e.text);
        CHECK(program.find("access_chain @gP 0x1", trace) > trace).dump("program", e.text);
        CHECK(program.count("access_chain @gP 0x0") == 1).dump("program", e.text);
        CHECK(program.count("access_chain @gP 0x1") == 1).dump("program", e.text);
        // the mask the trace takes is read where no call stands between, so it is read once
        CHECK(program.count("access_chain @gP 0x2") == 1).dump("program", e.text);
        // the launch's index is read again after the loop, for the address its traces stand before
        CHECK(program.count("stage_load e0") == 2).dump("program", e.text);
        // what the shader can write is read where the source reads it, and the payload once per trace
        CHECK(program.loads_from("@gP_data_1") == 2).dump("program", e.text);
        CHECK(program.count("= load @sgl_out") == 1).dump("program", e.text);
    }
}

TEST("sgl emit - SGL's own SPIR-V reads a constant block after the trace, and its writable buffer as the source does "
     "(EMIT-292)")
{
    auto const e = native_of("looped", sgl::emit::target::hlsl_vulkan);
    auto const walk = spirv_walk{.words = e.spirv};
    auto const trace = walk.find(k_op_trace_ray);
    REQUIRE(trace >= 0);
    auto const uniforms = walk.variables(k_uniform);
    // the block's fields `k` reads, after the trace; the mask before it, and the trace's acceleration address
    auto after = isize(0);
    for (auto const at : walk.chains_into(uniforms))
        after += at > trace ? 1 : 0;
    CHECK(after == 2);
    u32 const launch[] = {walk.built_in(k_launch_id)};
    CHECK(walk.loads_of(launch) == 2);
    CHECK(walk.loads_of(walk.variables(k_storage_buffer)) == 2);
    CHECK(walk.loads_of(walk.variables(k_ray_payload)) == 1);
}

TEST("sgl emit - a value read before a callable call is read again after it, on both native routes (EMIT-292)")
{
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const e = native_of("called", t);
        auto const program = program_lines(e.text);
        auto const call = program.find("execute_callable ");
        REQUIRE(call >= 0).dump("program", e.text);
        // `k` hands the callable its operand, and multiplies what it hands back
        CHECK(program.count("access_chain @gP 0x1") == 2).dump("program", e.text);
        CHECK(program.find("access_chain @gP 0x1", call) > call).dump("program", e.text);
        CHECK(program.count("stage_load e0") == 1).dump("program", e.text);
    }
    auto const e = native_of("called", sgl::emit::target::hlsl_vulkan);
    auto const walk = spirv_walk{.words = e.spirv};
    auto const call = walk.find(k_op_execute_callable);
    REQUIRE(call >= 0);
    auto after = isize(0);
    for (auto const at : walk.chains_into(walk.variables(k_uniform)))
        after += at > call ? 1 : 0;
    CHECK(after == 1);
}

TEST("sgl emit - a value no call point stands before its use is computed once (EMIT-292)")
{
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const e = native_of("straight", t);
        auto const program = program_lines(e.text);
        CHECK(program.count("access_chain @gP 0x1") == 1).dump("program", e.text);
        CHECK(program.count("stage_load e0") == 1).dump("program", e.text);
    }
}

TEST("sgl emit - a value is read again after a hit object's trace and after its invocation (EMIT-292)")
{
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const e = native_of("reordered", t, true);
        auto const program = program_lines(e.text);
        auto const traced = program.find("hit_object_trace ");
        REQUIRE(traced >= 0).dump("program", e.text);
        auto const invoked = program.find("hit_object_invoke ", traced);
        REQUIRE(invoked > traced).dump("program", e.text);
        // `seen` reads `k` between the two, and the store after both
        auto const first = program.find("access_chain @gP 0x1", traced);
        CHECK(first > traced).dump("program", e.text);
        CHECK(first < invoked).dump("program", e.text);
        CHECK(program.find("access_chain @gP 0x1", invoked) > invoked).dump("program", e.text);
        CHECK(program.count("access_chain @gP 0x1") == 2).dump("program", e.text);
        // the launch's index is read once, after both
        CHECK(program.count("stage_load e0") == 1).dump("program", e.text);
        CHECK(program.find("stage_load e0") > invoked).dump("program", e.text);
    }
}
