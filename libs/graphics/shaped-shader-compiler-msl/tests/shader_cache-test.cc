#include <blob-cache/blob_cache.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-shader-compiler-msl/all.hh>

// The shader_cache wraps ssc::msl::compiler in an async, hash-keyed get-or-create.
// Every test here runs on a host without the Metal toolchain too: the key and the tiers do not care which arm ran.

namespace
{
constexpr char const* k_kernel = R"(
#include <metal_stdlib>
using namespace metal;

struct work { device uint* values [[id(0)]]; };

#pragma sc numthreads 64 1 1
kernel void main0(constant work& w [[buffer(0)]], uint i [[thread_position_in_grid]]) { w.values[i] *= 2u; }
)";

ssc::msl::shader_description make_desc()
{
    return {.source = k_kernel, .entry_point = "main0", .stage = sg::shader_stage::compute};
}
} // namespace

ASYNC_TEST("ssc::msl shader_cache - compiles and resolves to a blob plus reflection")
{
    ssc::msl::shader_cache cache;
    cache.add_default_in_memory_provider();

    auto node = cache.compile(make_desc());
    REQUIRE(node != nullptr);

    sg::compiled_shader shader = co_await node;
    CHECK(shader.stage == sg::shader_stage::compute);
    CHECK(!shader.bytecode.empty());
    REQUIRE(shader.workgroup_size.has_value());
    CHECK(shader.workgroup_size.value().x == 64);
    REQUIRE(shader.bindings.size() == 1);
    CHECK(shader.bindings[0].name == cc::string_view("values"));
}

ASYNC_TEST("ssc::msl shader_cache - the same description returns the same node, and another option another one")
{
    ssc::msl::shader_cache cache;
    cache.add_default_in_memory_provider();

    auto a = cache.compile(make_desc());
    auto b = cache.compile(make_desc());
    CHECK(a.get() == b.get()); // one node, not a second compile

    auto c = cache.compile(make_desc(), {.defines = {"K=3"}});
    CHECK(c.get() != a.get());

    auto sized = make_desc();
    sized.workgroup_size = sg::compute_dimensions{.x = 8, .y = 8};
    auto d = cache.compile(sized);
    CHECK(d.get() != a.get());

    // Finished rather than abandoned mid-flight, which the run would report as leaked work.
    co_await cc::async_settled(a);
    co_await cc::async_settled(c);
    co_await cc::async_settled(d);
}

ASYNC_TEST("ssc::msl shader_cache - a compile error surfaces as an async error")
{
    ssc::msl::shader_cache cache;
    cache.add_default_in_memory_provider();

    auto node = cache.compile({.source = k_kernel, .entry_point = "absent"});
    auto const outcome = co_await cc::async_as_result(node);
    CHECK(outcome.has_error());
}

TEST("ssc::msl shader_cache - a compile persists across cache instances")
{
    if (!bcache::blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // Driven by hand, the way ssc::dxc's persistence test is: an unthreaded store pumped here, and one scheduler bound
    // here for the compile to resume on, bound before any compile since that decides where the work schedules.
    auto scheduler = cc::singlethreaded_scheduler();
    auto const scope = cc::async_worker_scope(scheduler);

    // A store of this test's own, because this test is ABOUT the store: it has to start empty and stay unshared.
    auto const path = cc::temp_file_path("ssc-msl-cache-test", ".db");
    auto store = bcache::blob_cache::create({.path = path, .unthreaded = true});

    auto const settle = [&](auto const& node)
    {
        CC_RECORD_SCOPE("msl_test.settle");

        while (!node->is_ready())
        {
            if (!cc::thread_pump_all())
                cc::this_thread_yield();
            scheduler.drain();
        }
        CHECK(node->is_ready());
    };

    auto const compile_once = [&]
    {
        // A fresh cache each time, so its in-memory tier is empty and the store is the only thing that can answer.
        ssc::msl::shader_cache cache;
        cache.add_default_in_memory_provider();
        cache.set_blob_cache(store.get());

        auto node = cache.compile(make_desc());
        settle(node);
        auto const* const value = node->try_value();
        return value != nullptr ? *value : sg::compiled_shader();
    };

    auto const first = compile_once();
    CHECK(!first.bytecode.empty());

    // The store is fire-and-forget, so the entry exists once the actor has drained past it.
    settle(store->flush());
    CHECK(store->get_stats().puts_stored == 1);

    // A hit is the proof no second compile ran, so no second `metal` was spawned where the toolchain is there.
    auto const second = compile_once();
    CHECK(store->get_stats().hits >= 1);

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
