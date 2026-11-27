#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

using namespace cc::primitive_defines;

// EMIT-292 in SGL's DXIL, as DXC disassembles it: what reads only constants and the launch is read again after each call
// point, through the writer's own passes, which would otherwise make the two reads one.

namespace
{
constexpr cc::string_view k_source = R"(require raytracing_pipeline

binding gP:
    tint: float3
    scale: float
    mask: uint
    world: acceleration_structure[.triangles]
    data: mut buffer[float4]
    fixed: buffer[float4]

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

@raygen fun reordered(@launch_id id: int3){gP}:
    let k = gP.scale * 2.0
    let mut p = payload(float3(0.0))
    const if has(.shader_execution_reordering):
        let h = trace_hit(gP.world, ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)), R.surface, mut p)
        let seen = k + 1.0
        h.invoke(mut p)
        gP.data[id.x] = float4(p.color.x * k, seen, 0.0, 1.0)

@raygen fun adjacent(@launch_id id: int3){gP}:
    let a = gP.fixed[0].x
    let mut p = payload(float3(a, 0.0, 0.0))
    trace(gP.world, ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)), R.surface, mut p)
    gP.data[0] = float4(p.color.x, gP.fixed[0].y, 0.0, 1.0)

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

enum class reordering
{
    none,
    native,
    nvapi,
};

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> native_of(cc::string_view entry, reordering how)
{
    auto device = sgl::check::feature_set();
    if (how != reordering::none)
        device.set(sgl::check::feature::shader_execution_reordering);
    sgl::check::feature_fill const nvapi[]
        = {{.f = sgl::check::feature::shader_execution_reordering, .how = sgl::check::fill::nvapi}};
    return sgl::compile_to_text({.source = k_source,
                                 .entry_point = entry,
                                 .target = sgl::emit::target::hlsl_dx12,
                                 .device_features = device,
                                 .fills = how == reordering::nvapi ? cc::span<sgl::check::feature_fill const>(nvapi)
                                                                   : cc::span<sgl::check::feature_fill const>(),
                                 .native = sgl::emit::native_route::require});
}

/// The lines of the entry point's body that call a `dx.op` named `op`, by their position among the body's lines.
struct body
{
    cc::vector<cc::string> lines;

    explicit body(cc::string_view disassembly)
    {
        auto inside = false;
        auto at = isize(0);
        while (at < disassembly.size())
        {
            auto end = disassembly.find('\n', at);
            if (end < 0)
                end = disassembly.size();
            auto const line = disassembly.subview({.offset = at, .size = end - at});
            at = end + 1;
            if (line.starts_with("define "))
                inside = true;
            else if (line.starts_with("}"))
                inside = false;
            else if (inside)
                lines.push_back(cc::string(line));
        }
    }

    [[nodiscard]] cc::vector<isize> calls_of(cc::string_view op) const
    {
        auto const callee = cc::format("@dx.op.{}", op);
        auto found = cc::vector<isize>();
        for (auto i = isize(0); i < lines.size(); ++i)
            if (lines[i].contains(callee))
                found.push_back(i);
        return found;
    }
    /// The calls of `op` standing after line `from` and before line `to`.
    [[nodiscard]] isize between(cc::string_view op, isize from, isize to) const
    {
        auto n = isize(0);
        for (auto const i : calls_of(op))
            n += i > from && i < to ? 1 : 0;
        return n;
    }
    [[nodiscard]] cc::string text() const
    {
        auto out = cc::string();
        for (auto const& l : lines)
            out.appendf("{}\n", l);
        return out;
    }
};

[[nodiscard]] body disassembled(ssc::dxc::compiler& compiler, cc::string_view entry, reordering how)
{
    auto const e = native_of(entry, how);
    REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
    auto const refused = compiler.validate(e.value().dxil);
    CHECK(!refused.has_value()).dump("entry", entry).dump("validator", refused.value_or(cc::string()));
    auto const text = compiler.disassemble(e.value().dxil);
    REQUIRE(text.has_value());
    return body(text.value());
}
} // namespace

TEST("slib - SGL's DXIL reads a constant block's row again after each trace, and once between two (EMIT-292)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
#ifndef CC_OS_WINDOWS
    SKIP("DXC compiles DXIL only on Windows, where its reflection is");
#endif
    auto const b = disassembled(compiler.value(), "looped", reordering::none);
    auto const traces = b.calls_of("traceRay.");
    REQUIRE(traces.size() == 1).dump("body", b.text());
    auto const trace = traces[0];
    // `tint` and `scale` share a row, read after the trace as one; the mask's row before it, in the same block
    CHECK(b.between("cbufferLoadLegacy", trace, b.lines.size()) == 1).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", -1, trace) == 1).dump("body", b.text());
    // the launch's index read again after the loop, for the addresses it computes there
    CHECK(b.between("dispatchRaysIndex", trace, b.lines.size()) == 2).dump("body", b.text());
    // what the shader can write is read where the source reads it
    CHECK(b.calls_of("rawBufferLoad").size() == 2).dump("body", b.text());
}

TEST("slib - SGL's DXIL reads a constant block again after a callable call (EMIT-292)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
#ifndef CC_OS_WINDOWS
    SKIP("DXC compiles DXIL only on Windows, where its reflection is");
#endif
    auto const b = disassembled(compiler.value(), "called", reordering::none);
    auto const calls = b.calls_of("callShader.");
    REQUIRE(calls.size() == 1).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", -1, calls[0]) == 1).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", calls[0], b.lines.size()) == 1).dump("body", b.text());
    CHECK(b.between("dispatchRaysIndex", calls[0], b.lines.size()) == 1).dump("body", b.text());
}

TEST("slib - SGL's DXIL reads a constant block again after a hit object's trace and its invocation (EMIT-292)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
#ifndef CC_OS_WINDOWS
    SKIP("DXC compiles DXIL only on Windows, where its reflection is");
#endif
    auto const b = disassembled(compiler.value(), "reordered", reordering::native);
    auto const traced = b.calls_of("hitObject_TraceRay");
    auto const invoked = b.calls_of("hitObject_Invoke");
    REQUIRE(traced.size() == 1).dump("body", b.text());
    REQUIRE(invoked.size() == 1).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", -1, traced[0]) == 0).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", traced[0], invoked[0]) == 1).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", invoked[0], b.lines.size()) == 1).dump("body", b.text());
}

TEST("slib - SGL's DXIL of an nvapi fill reads a constant block again after the trace it pins (EMIT-292)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
#ifndef CC_OS_WINDOWS
    SKIP("DXC compiles DXIL only on Windows, where its reflection is");
#endif
    // the trace a hit object's record pins and the invocation's are both DXIL's TraceRay under NVAPI's protocol
    auto const b = disassembled(compiler.value(), "reordered", reordering::nvapi);
    auto const traces = b.calls_of("traceRay.");
    REQUIRE(traces.size() == 2).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", -1, traces[0]) == 0).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", traces[0], traces[1]) == 1).dump("body", b.text());
    CHECK(b.between("cbufferLoadLegacy", traces[1], b.lines.size()) == 1).dump("body", b.text());
}

TEST("slib - SGL's DXIL joins no two lanes of a read-only buffer across a trace (EMIT-292)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
#ifndef CC_OS_WINDOWS
    SKIP("DXC compiles DXIL only on Windows, where its reflection is");
#endif
    // the two lanes follow on in one element, which one load reads where no call point stands between
    auto const b = disassembled(compiler.value(), "adjacent", reordering::none);
    auto const traces = b.calls_of("traceRay.");
    REQUIRE(traces.size() == 1).dump("body", b.text());
    CHECK(b.between("rawBufferLoad", -1, traces[0]) == 1).dump("body", b.text());
    CHECK(b.between("rawBufferLoad", traces[0], b.lines.size()) == 1).dump("body", b.text());
}
