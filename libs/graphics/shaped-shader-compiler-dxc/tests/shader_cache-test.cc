#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

// The shader_cache wraps ssc::dxc::compiler in an async, hash-keyed get-or-create: the same
// (description, options) returns the same async node without recompiling. No async pool is installed
// here, so the scheduled node is driven inline by cc::async_blocking_get_singlethreaded.

namespace
{
constexpr char const* double_compute_hlsl = R"(
RWStructuredBuffer<uint> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[tid.x] = tid.x * 2u;
}
)";

ssc::dxc::shader_description make_desc()
{
    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    desc.entry_point = "main";
    desc.model = ssc::dxc::shader_model::sm_6_8;
    desc.source = double_compute_hlsl;
    return desc;
}
} // namespace

TEST("ssc::dxc shader_cache - compiles and resolves to bytecode + reflection")
{
    ssc::dxc::shader_cache cache;
    cache.add_default_in_memory_provider();

    auto async_shader = cache.compile(make_desc());
    REQUIRE(async_shader != nullptr);

    sg::compiled_shader shader = cc::async_blocking_get_singlethreaded(async_shader);
    CHECK(shader.stage == sg::shader_stage::compute);
    CHECK(shader.format == sg::shader_format::dxil);
    CHECK(!shader.bytecode.empty());
    REQUIRE(shader.workgroup_size.has_value());
    CHECK(shader.workgroup_size.value().x == 64);
    REQUIRE(shader.bindings.size() == 1);
    CHECK(shader.bindings[0].name == cc::string_view("Output"));
}

TEST("ssc::dxc shader_cache - same key returns the same async node")
{
    ssc::dxc::shader_cache cache;
    cache.add_default_in_memory_provider();

    auto a = cache.compile(make_desc());
    auto b = cache.compile(make_desc()); // identical description -> cache hit

    CHECK(a.get() == b.get()); // same node, not a second compilation

    // a different entry of the identity diverges
    auto altered = make_desc();
    altered.entry_point = "main"; // same
    ssc::dxc::compile_options opts;
    opts.debug_info = true; // different options -> different key
    auto c = cache.compile(altered, opts);
    CHECK(c.get() != a.get());
}

TEST("ssc::dxc shader_cache - a compile error surfaces as an async error")
{
    ssc::dxc::shader_cache cache;
    cache.add_default_in_memory_provider();

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    desc.source = "[numthreads(1,1,1)] void main() { this is not valid HLSL }";

    auto async_shader = cache.compile(desc);
    auto result = cc::try_async_blocking_get_singlethreaded(async_shader);
    CHECK(result.has_error());
}
