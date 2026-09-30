#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/compiler/metal_compiler.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <slib_test_sgl_shaders.hh>

#if SLIB_HAS_METAL

// The metal edge, and the one thing it exists for: an SGL package reaching metal.
// Both arms of the compiler behind it produce something the metal backend accepts, so these assert what the edge
// resolves and what comes out, not which arm ran — that is a property of the host, not of the package.
// A compile runs on the scheduler rather than inside the call that asked for it, so every node is settled before it is read.

namespace
{
/// The value of a SETTLED node.
sg::compiled_shader const& value_of(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    if (shader->has_error())
        FAIL(shader->try_error()->underlying().to_string());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}
/// Emits `entry` of `source` as MSL and compiles it, returning the settled node.
sg::async_compiled_shader compile_sgl(slib::shader_compiler const& sgl,
                                      cc::string_view source,
                                      cc::string_view entry,
                                      sg::shader_stage stage)
{
    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };
    auto const preprocessed
        = sgl.preprocess({.source = source, .entry_point = entry, .stage = stage, .label = "probe.sgl"}, resolve);
    if (preprocessed.has_error())
        FAIL(preprocessed.error().to_string());
    return sgl.compile(
        {.source = preprocessed.value().source, .entry_point = preprocessed.value().entry_point, .stage = stage});
}

/// Every texture method in every shape it has, each written once: what no fixture draws with reaches a Metal compiler here.
constexpr cc::string_view k_texture_probe = R"sgl(require multisampled_array_textures

binding tex:
    t1: texture_1d[float4]
    t1a: texture_1d_array[float4]
    t2: texture_2d[float4]
    t2a: texture_2d_array[float4]
    t3: texture_3d[float4]
    tc: texture_cube[float4]
    tca: texture_cube_array[float4]
    tms: texture_2d_ms[float4]
    tmsa: texture_2d_ms_array[float4]
    d2: texture_2d_depth
    d2a: texture_2d_array_depth
    dc: texture_cube_depth
    dca: texture_cube_array_depth
    dms: texture_2d_ms_depth
    ti: texture_2d[int4]
    tu: texture_2d[uint]
    smp: sampler
    @non_filtering nf: sampler
    cmp: comparison_sampler

struct pixel_input:
    @position position: hpos4
    uv: float2

@pixel struct target:
    color: float4

@pixel fun ps(p: pixel_input){tex} -> target:
    let uv = p.uv
    let uvw = float3(uv.x, uv.y, 0.5)
    let xy = int2(3, 4)
    let mut acc = float4(0.0, 0.0, 0.0, 0.0)
    acc += tex.t1.sample(uv.x, tex.smp)
    acc += tex.t1.sample(uv.x, tex.smp, level = 1.0)
    acc += tex.t1.sample(uv.x, tex.smp, bias = 0.5)
    acc += tex.t1.sample(uv.x, tex.smp, grad_x = 0.1, grad_y = 0.1)
    acc += tex.t1.load(3, 0)
    acc += tex.t1a.sample(uv.x, tex.smp, layer = 1)
    acc += tex.t1a.sample(uv.x, tex.smp, layer = 1, level = 1.0)
    acc += tex.t1a.load(3, 0, layer = 1)
    acc += tex.t2.sample(uv, tex.smp)
    acc += tex.t2.sample(uv, tex.smp, offset = int2(1, 0))
    acc += tex.t2.sample(uv, tex.smp, level = 1.0)
    acc += tex.t2.sample(uv, tex.smp, level = 1.0, offset = int2(1, 0))
    acc += tex.t2.sample(uv, tex.smp, bias = 0.5)
    acc += tex.t2.sample(uv, tex.smp, grad_x = uv, grad_y = uv)
    acc += tex.t2.gather(uv, tex.smp)
    acc += tex.t2.gather(uv, tex.smp, component = texel_component.y)
    acc += tex.t2.gather(uv, tex.smp, component = texel_component.w, offset = int2(1, 1))
    acc += tex.t2.load(xy, 1)
    acc += tex.t2a.sample(uv, tex.smp, layer = 2)
    acc += tex.t2a.sample(uv, tex.smp, layer = 2, level = 1.0)
    acc += tex.t2a.sample(uv, tex.smp, layer = 2, bias = 0.5)
    acc += tex.t2a.gather(uv, tex.smp, layer = 2)
    acc += tex.t2a.load(xy, 0, layer = 2)
    acc += tex.t3.sample(uvw, tex.smp)
    acc += tex.t3.sample(uvw, tex.smp, level = 1.0)
    acc += tex.t3.load(int3(1, 2, 3), 0)
    acc += tex.tc.sample(uvw, tex.smp)
    acc += tex.tc.sample(uvw, tex.smp, level = 1.0)
    acc += tex.tc.gather(uvw, tex.smp)
    acc += tex.tca.sample(uvw, tex.smp, layer = 1)
    acc += tex.tca.sample(uvw, tex.smp, layer = 1, level = 1.0)
    acc += tex.tms.load(xy, sample = 1)
    acc += tex.tmsa.load(xy, sample = 1, layer = 1)
    let mut d = 0.0
    d += tex.d2.sample(uv, tex.nf)
    d += tex.d2.sample(uv, tex.nf, level = 1)
    d += tex.d2.sample_compare(uv, tex.cmp, reference = 0.5)
    d += tex.d2.sample_compare(uv, tex.cmp, reference = 0.5, level = 0.0)
    d += tex.d2.load(xy, 0)
    d += tex.d2.gather(uv, tex.nf).x
    d += tex.d2.gather_compare(uv, tex.cmp, reference = 0.5).y
    d += tex.d2a.sample(uv, tex.nf, layer = 1)
    d += tex.d2a.sample_compare(uv, tex.cmp, layer = 1, reference = 0.5)
    d += tex.d2a.load(xy, 0, layer = 1)
    d += tex.d2a.gather_compare(uv, tex.cmp, layer = 1, reference = 0.5).z
    d += tex.dc.sample(uvw, tex.nf)
    d += tex.dc.sample_compare(uvw, tex.cmp, reference = 0.5)
    d += tex.dc.gather(uvw, tex.nf).w
    d += tex.dca.sample(uvw, tex.nf, layer = 1)
    d += tex.dca.sample_compare(uvw, tex.cmp, layer = 1, reference = 0.5, level = 0.0)
    d += tex.dms.load(xy, sample = 0)
    let i = tex.ti.load(xy, 0)
    let u = tex.tu.load(xy, 0)
    let s = tex.t1.size(0) + tex.t1a.size(0) + tex.t1a.layer_count() + tex.t1.level_count()
    let s2 = tex.t2.size(1) + tex.t2a.size(0) + tex.tc.size(0) + tex.tca.size(0) + tex.tms.size() + tex.tmsa.size() + tex.d2.size(0) + tex.dms.size()
    let s3 = tex.t3.size(0)
    let n = tex.t2a.layer_count() + tex.tca.layer_count() + tex.tmsa.layer_count() + tex.d2a.layer_count() + tex.dca.layer_count() + tex.t2.level_count() + tex.t3.level_count() + tex.tc.level_count() + tex.tms.sample_count() + tex.dms.sample_count() + tex.tmsa.sample_count()
    let k = ((s + s2.x + s3.z + n + i.x) as float) + (u as float) + d
    return {color = acc + float4(k, k, k, k)}
)sgl";

/// Every image shape loaded and stored, with its size and layer count.
constexpr cc::string_view k_image_probe = R"sgl(require readwrite_image_formats

binding img:
    a: mut image_1d[.rgba8_unorm]
    b: mut image_1d_array[.r32_float]
    c: mut image_2d[.rgba16_float]
    d: mut image_2d_array[.r32_uint]
    e: mut image_3d[.r32_sint]
    f: out image_2d[.rgba8_unorm]
    g: image_2d[.rgba32_float]

@compute(8, 8) fun cs(@thread_id id: int3){img}:
    let xy = int2(id.x, id.y)
    img.a.store(id.x, img.a.load(id.x) * 0.5)
    img.b.store(id.x, img.b.load(id.x, layer = 1) + 1.0, layer = 0)
    img.c.store(xy, img.c.load(xy) + img.g.load(xy))
    img.d.store(xy, img.d.load(xy, layer = 2) + (1 as uint), layer = 1)
    img.e.store(int3(id.x, id.y, id.z), img.e.load(int3(1, 2, 3)) - 1)
    img.f.store(xy, float4(1.0, 0.0, 0.0, 1.0))
    let s = img.a.size() + img.b.size() + img.b.layer_count() + img.c.size().x + img.d.size().y + img.d.layer_count() + img.e.size().z + img.g.size().x
    img.a.store(0, float4(s as float, 0.0, 0.0, 0.0))
)sgl";
} // namespace

TEST("slib metal compiler - the edge is metal source to a metal library")
{
    auto const compiler = slib::create_metal_compiler();
    REQUIRE(compiler != nullptr);
    CHECK(compiler->source_language() == slib::shader_language::metal);
    CHECK(compiler->target_format() == sg::shader_format::metal_lib);
}

TEST("slib metal compiler - preprocess hands the text back, because MSL needs no flattening here")
{
    auto const compiler = slib::create_metal_compiler();
    REQUIRE(compiler != nullptr);

    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };
    auto const text = compiler->preprocess({.source = "kernel void k() {}", .entry_point = "k"}, resolve);
    REQUIRE(text.has_value());
    CHECK(text.value().source == "kernel void k() {}");
}

ASYNC_TEST("slib metal compiler - MSL compiles, and its bindings are reflected out of the text")
{
    auto const compiler = slib::create_metal_compiler();
    REQUIRE(compiler != nullptr);

    auto const shader = compiler->compile({.source = R"(
#include <metal_stdlib>
using namespace metal;
struct frame { device uint* values [[id(0)]]; };
kernel void main0(device frame& f [[buffer(0)]], uint t [[thread_position_in_grid]]) { f.values[t] *= 2u; }
)",
                                           .entry_point = "main0",
                                           .stage = sg::shader_stage::compute});
    co_await cc::async_settled(shader);

    auto const& compiled = value_of(shader);
    CHECK(compiled.entry_point == "main0");
    REQUIRE(compiled.bindings.size() == 1);
    CHECK(compiled.bindings[0].name == "values");
    CHECK(compiled.bindings[0].type == sg::binding_type::buffer);

    // A metallib where this host has the toolchain, MSL source where it does not — the backend takes either.
    auto const is_metal_format
        = compiled.format == sg::shader_format::metal_lib || compiled.format == sg::shader_format::msl;
    CHECK(is_metal_format);
}

ASYNC_TEST("slib metal compiler - the same source twice is one node, not a second compile")
{
    auto const compiler = slib::create_metal_compiler();
    REQUIRE(compiler != nullptr);

    auto const desc = slib::shader_source_description{
        .source = R"(
struct w { device uint* v [[id(0)]]; };
kernel void k(constant w& b [[buffer(0)]]) { (void)b; }
)",
        .entry_point = "k",
        .stage = sg::shader_stage::compute,
    };
    auto const first = compiler->compile(desc);
    auto const second = compiler->compile(desc);
    CHECK(first.get() == second.get());
    co_await cc::async_settled(first);
}

ASYNC_TEST("slib metal compiler - the sgl edge carries a package to metal", exclusive("slib-shader-library"))
{
    // The point of the whole arm: one SGL source, compiled for metal like any other target.
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_metal_compiler()));
    lib.add_package(slib_test::sgl_shaders::package());

    auto const vs_node = slib_test::sgl_shaders::cube.main_vs->acquire(sg::shader_format::metal_lib);
    auto const ps_node = slib_test::sgl_shaders::cube.main_ps->acquire(sg::shader_format::metal_lib);
    co_await cc::async_settled(vs_node);
    co_await cc::async_settled(ps_node);

    auto const& vs = value_of(vs_node);
    CHECK(vs.stage == sg::shader_stage::vertex);
    CHECK(vs.entry_point == "main_vs");

    auto const& ps = value_of(ps_node);
    CHECK(ps.stage == sg::shader_stage::fragment);
}

ASYNC_TEST("slib metal compiler - an SGL struct named like one of Metal's global names compiles")
{
    // `quad` is a typedef Metal declares at global scope, and `add_const` one `using namespace metal;` brings there.
    // Both are reserved in MSL, so the text mints the struct a name of its own and neither is ambiguous.
    auto const sgl = slib::create_sgl_compiler(slib::create_metal_compiler());
    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };
    auto const preprocessed = sgl->preprocess({.source = "struct quad:\n"
                                                         "    a: float\n"
                                                         "\n"
                                                         "struct add_const:\n"
                                                         "    b: float\n"
                                                         "\n"
                                                         "binding work:\n"
                                                         "    values: mut buffer[quad]\n"
                                                         "\n"
                                                         "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                                                         "    let c = add_const(2.0)\n"
                                                         "    work.values[id.x] = quad(work.values[id.x].a * c.b)\n",
                                               .entry_point = "cs",
                                               .stage = sg::shader_stage::compute,
                                               .label = "quad.sgl"},
                                              resolve);
    REQUIRE(preprocessed.has_value());

    auto const shader = sgl->compile({.source = preprocessed.value().source,
                                      .entry_point = preprocessed.value().entry_point,
                                      .stage = sg::shader_stage::compute});
    co_await cc::async_settled(shader);
    auto const& compiled = value_of(shader);
    CHECK(compiled.entry_point == "cs");
    REQUIRE(compiled.bindings.size() == 1);
    CHECK(compiled.bindings[0].type == sg::binding_type::buffer);
    CHECK(compiled.bindings[0].group_index == 0u);
}

ASYNC_TEST("slib metal compiler - every texture and image method SGL writes is MSL the Metal compiler takes")
{
    auto const sgl = slib::create_sgl_compiler(slib::create_metal_compiler());
    auto const pixel = compile_sgl(*sgl, k_texture_probe, "ps", sg::shader_stage::fragment);
    auto const compute = compile_sgl(*sgl, k_image_probe, "cs", sg::shader_stage::compute);
    co_await cc::async_settled(pixel);
    co_await cc::async_settled(compute);

    // Without the toolchain the text travels as source, and nothing compiles it until a pipeline is built.
    if (value_of(pixel).format != sg::shader_format::metal_lib)
        SKIP("no Metal toolchain on this host, so nothing compiled the text");
    CHECK(value_of(compute).format == sg::shader_format::metal_lib);
}

#endif
