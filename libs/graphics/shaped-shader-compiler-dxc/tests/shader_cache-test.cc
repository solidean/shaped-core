#include <blob-cache/blob_cache.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

// The shader_cache wraps ssc::dxc::compiler in an async, hash-keyed get-or-create: the same (description, options) returns the same async node without recompiling.
// No async pool is installed here, so the scheduled node is driven inline by cc::async_blocking_get_singlethreaded.

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
    auto const outcome = cc::try_async_blocking_get_singlethreaded(async_shader);
    REQUIRE(outcome.has_value()); // the graph completed
    CHECK(outcome.value().has_error());
}

TEST("ssc::dxc shader_cache - a compile persists across cache instances")
{
    if (!bcache::blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // Driven by hand rather than by a thread pool, and the same way in every build.
    // This test is about what the store remembers, so its message order is the test's own: an unthreaded actor pumped
    // here, and one scheduler bound here for the compile to resume on.
    // The scope has to be bound before any compile, since that is what decides where the work schedules.
    auto scheduler = cc::singlethreaded_scheduler();
    auto const scope = cc::async_worker_scope(scheduler);

    // A store of this test's own, because this test is ABOUT the store: it has to start empty and stay unshared.
    auto const path = cc::temp_file_path("ssc-dxc-cache-test", ".db");
    auto store = bcache::blob_cache::create({.path = path, .unthreaded = true});

    // Two drivers, both needed: the pump resolves what the compile is parked on, the drain resumes the compile.
    // Bounded, so a compile that can never finish fails the test instead of hanging it.
    auto const settle = [&](auto const& node)
    {
        for (auto i = 0; i < 100000 && !node->is_ready(); ++i)
        {
            (void)store->pump();
            scheduler.drain();
        }
        CHECK(node->is_ready());
    };

    auto const compile_once = [&]
    {
        // A fresh cache each time, so its in-memory tier is empty and the store is the only thing that can answer.
        ssc::dxc::shader_cache cache;
        cache.add_default_in_memory_provider();
        cache.set_blob_cache(store.get());

        auto node = cache.compile(make_desc());
        settle(node);
        auto const* const value = node->try_value();
        return value != nullptr ? *value : sg::compiled_shader();
    };

    auto const first = compile_once();
    CHECK(!first.bytecode.empty());

    // The store is fire-and-forget, so the entry exists once the actor has drained past it, not once compile returned.
    // One mailbox, so a flush queued after the put is processed after it.
    settle(store->flush());
    CHECK(store->get_stats().puts_stored == 1); // nothing was there, so the compile was stored

    auto const second = compile_once();
    CHECK(store->get_stats().hits >= 1); // ...and the second compile did not have to run

    // Decoded, not recompiled — so every field has to have survived the round trip.
    CHECK(second.stage == first.stage);
    CHECK(second.format == first.format);
    CHECK(second.entry_point == first.entry_point);
    CHECK(second.bytecode.size() == first.bytecode.size());
    REQUIRE(second.bindings.size() == first.bindings.size());
    CHECK(second.bindings[0].name == first.bindings[0].name);
    REQUIRE(second.workgroup_size.has_value());
    CHECK(second.workgroup_size.value().x == first.workgroup_size.value().x);
    CHECK(second.compiler.version == first.compiler.version);

    store->close();
    store = nullptr;
    (void)cc::remove_file(path);
    (void)cc::remove_file(cc::format("{}-wal", path));
    (void)cc::remove_file(cc::format("{}-shm", path));
}
