#include <clean-core/common/assert-handler.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>

// The test target declares this package itself (sc_add_shader_package in the CMakeLists); generated into the build dir and private to this binary.
#include <sg_test_shaders.hh>

using namespace cc::primitive_defines;

// Render-routine framework tests, backend-agnostic: each is an INVOCABLE_TEST taking a live context, so it runs
// against every backend the binary was built with (see tests/context/context-test.cc for the harness).
//
// Three things are proven here:
//   1. the framework's phase orchestration re-runs init on a reload, but not init_once,
//   2. eviction really drops an instance, and the per-thread acquire cache does not resurrect it, and
//   3. a real routine compiles a compute shader through slib and dispatches it end to end.
//
// **Every test owns its routine type.** A routine instance is per-context and these tests share one context per
// backend, so two tests reaching for the same type would see each other's phase counts — and the one that ran second
// would assert against a routine that was already initialized.
// Tests needing TWO contexts live in routine-contexts-test.cc, which cannot be backend-agnostic for that reason.

namespace
{
// Records how often each phase ran; does no GPU work.
// One per test that counts phases, since the count is per (type, context) and the context is shared.
template <int Tag>
class counting_routine : public sg::render_routine<counting_routine<Tag>>
{
public:
    int once = 0;
    int inits = 0;

protected:
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope) override
    {
        ++once;
        co_return;
    }
    cc::shared_async<cc::unit> init(sg::routine_init_scope) override
    {
        ++inits;
        co_return;
    }
};

using phases_routine = counting_routine<0>;
using evict_routine = counting_routine<1>;

// Like counting_routine, but counted atomically so racing acquires can be checked.
// The counters are static so the test can read them after the race without a handle to the per-context instance.
class racing_routine : public sg::render_routine<racing_routine>
{
public:
    static inline cc::atomic<int> once = 0;
    static inline cc::atomic<int> inits = 0;

protected:
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope) override
    {
        ++once;
        co_return;
    }
    // The sleep widens the window a racing second registration would slip through, so the test below actually
    // exercises the single-initialization rule rather than passing because the first thread happened to finish first.
    cc::shared_async<cc::unit> init(sg::routine_init_scope) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ++inits;
        co_return;
    }
};

// Counts increments made through acquire_exclusive, deliberately with a PLAIN int rather than an atomic.
// The guard is what must make the read-modify-write exclusive, and an atomic would pass with or without it.
template <int Tag>
class counter_routine : public sg::render_routine<counter_routine<Tag>>
{
public:
    // The tests below tick before racing, so a pending routine here would be the test's bug rather than the guard's.
    static void bump(sg::command_list& cmd)
    {
        auto self = counter_routine::try_acquire_exclusive(cmd);
        CC_ASSERT(self.is_ready(), "tick the routine system before racing this");
        ++self->_count;
    }

    [[nodiscard]] static int count_of(sg::command_list& cmd)
    {
        auto self = counter_routine::try_acquire_exclusive(cmd);
        CC_ASSERT(self.is_ready(), "tick the routine system first");
        return self->_count;
    }

    // Reads the same member through the read-only scope, which only compiles because it does not mutate.
    [[nodiscard]] static int count_via_const(sg::command_list& cmd)
    {
        auto const self = counter_routine::try_acquire(cmd);
        CC_ASSERT(self.is_ready(), "tick the routine system first");
        return self->_count;
    }

private:
    int _count = 0;
};

using guard_routine = counter_routine<0>;
using racing_counter_routine = counter_routine<1>;

// A routine parametrized on a RUNTIME value: the template names the parameter's type, the value is passed at acquire.
// One instance per distinct value, so each sees its own params() and runs its own phases.
class formatted_routine : public sg::render_routine<formatted_routine, sg::pixel_format>
{
public:
    int inits = 0;
    sg::pixel_format seen = sg::pixel_format::undefined;

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope) override
    {
        ++inits;
        seen = params();
        co_return;
    }
};

// Slow enough that a tick with a small budget stops after one of them.
// Two distinct types rather than two parametrizations, so the budget test does not depend on iteration order within
// one type being stable.
template <int Tag>
class slow_routine : public sg::render_routine<slow_routine<Tag>>
{
public:
    bool ran = false;

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ran = true;
        co_return;
    }
};

// The end-to-end routine: owns its pipeline, built in init and dispatched in execute.
// Reached by type — no handle, no registration call.
class pattern_fill_routine : public sg::render_routine<pattern_fill_routine>
{
public:
    static void execute(sg::command_list& cmd, sg::buffer<u32> const& out)
    {
        auto const self = try_acquire(cmd);
        CC_ASSERT(self.is_ready(), "tick the routine system before dispatching this");
        CC_ASSERT(self->_pipeline != nullptr, "pattern_fill routine failed to initialize");

        // Polled, not waited on: init awaited the build, so a ready routine has a ready pipeline.
        auto const* const pipeline = self->_pipeline->try_value();
        CC_ASSERT(pipeline != nullptr && *pipeline != nullptr, "a ready routine must have a built pipeline");

        auto const group = cmd.context().transient.create_binding_group(
            self->_group_layout, {{.name = "gValues", .view = out.as_readwrite_buffer()}});

        cmd.compute.bind_pipeline(**pipeline);
        cmd.compute.bind_group(0, *group);
        cmd.compute.dispatch_threads(out.element_count());
    }

    [[nodiscard]] static bool is_usable(sg::command_list& cmd)
    {
        auto const self = try_acquire(cmd);
        return self.is_ready() && self->_pipeline != nullptr;
    }

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override
    {
        auto& ctx = scope.context();

        auto const shader = sg::test::shaders::pattern_fill.compute.main->acquire(ctx);
        co_await cc::async_settled(shader);
        auto const* const compiled = shader->try_value();
        if (compiled == nullptr)
            co_return; // the context cannot produce a format we can use; is_usable() then reports it

        _group_layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
        auto const layout
            = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {_group_layout}});
        _pipeline = ctx.cached.acquire_compute_pipeline(
            sg::compute_pipeline_description{.shader = *compiled, .layout = layout});
        co_await cc::async_settled(_pipeline);
    }

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};
} // namespace

INVOCABLE_TEST("sg - try_acquire hands out a read-only scope, try_acquire_exclusive a move-only guard",
               (sg::context_handle const& ctx))
{
    // A mutable reference creeping back into the unlocked path would silently re-open unguarded writes.
    static_assert(std::is_same_v<decltype(guard_routine::try_acquire(std::declval<sg::command_list&>())),
                                 sg::routine_scope<guard_routine>>);
    static_assert(std::is_same_v<decltype(guard_routine::try_acquire_exclusive(std::declval<sg::command_list&>())),
                                 sg::routine_guard<guard_routine>>);
    // Both are move-only: a copy would be a second holder of a lock, or of a readiness that was true when it was taken.
    static_assert(!std::is_copy_constructible_v<sg::routine_guard<guard_routine>>);
    static_assert(!std::is_copy_constructible_v<sg::routine_scope<guard_routine>>);

    REQUIRE(ctx != nullptr);
    auto cmd = ctx->create_command_list();

    // Registered by asking, brought up by the tick — the helpers below assert readiness rather than waiting for it.
    (void)guard_routine::try_acquire(*cmd);
    (void)ctx->routines.tick_until_idle();

    // Both entry points reach the one per-context instance — a write through the guard is what the read-only scope then sees.
    auto const before = guard_routine::count_of(*cmd);
    guard_routine::bump(*cmd);
    CHECK(guard_routine::count_of(*cmd) == before + 1);
    CHECK(guard_routine::count_via_const(*cmd) == before + 1);

    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg - routine phases run once, then re-run init on a reload",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto cmd = ctx->create_command_list();

    // Asking registers it; the TICK is what runs the phases.
    (void)phases_routine::try_acquire(*cmd);
    (void)ctx->routines.tick_until_idle();

    auto const first = phases_routine::try_acquire(*cmd);
    REQUIRE(first.is_ready());
    CHECK(first->once == 1);
    CHECK(first->inits == 1);

    // A second tick at the same generation changes nothing (same per-context instance).
    (void)ctx->routines.tick_until_idle();
    CHECK(first->once == 1);
    CHECK(first->inits == 1);

    // A reload bumps the global generation: init re-runs, init_once does not.
    sg::signal_reload();
    (void)ctx->routines.tick_until_idle();
    CHECK(first->once == 1);
    CHECK(first->inits == 2);

    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg - evicting a routine drops its instance (the acquire cache does not resurrect it)",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto cmd = ctx->create_command_list();

    (void)evict_routine::try_acquire(*cmd);
    (void)ctx->routines.tick_until_idle();
    auto const first = evict_routine::try_acquire(*cmd);
    REQUIRE(first.is_ready());
    CHECK(first->once == 1);

    // Drive the first instance's init count to 2, so it is distinguishable from a fresh one.
    sg::signal_reload();
    (void)ctx->routines.tick_until_idle();
    CHECK(first->inits == 2);

    evict_routine::evict(*ctx);

    // A fresh instance, built from scratch: every phase back at 1.
    // A cached slot that survived the eviction would instead hand back the old object (inits == 2) — or worse, a freed one.
    (void)evict_routine::try_acquire(*cmd);
    (void)ctx->routines.tick_until_idle();
    auto const second = evict_routine::try_acquire(*cmd);
    REQUIRE(second.is_ready());
    CHECK(second->once == 1);
    CHECK(second->inits == 1);

    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg - a routine compiles a shader and dispatches it end to end",
               (sg::context_handle const& ctx),
               exclusive("slib-shader-library"))
{
    REQUIRE(ctx != nullptr);

    // Both compilers are registered and the asset picks between them by asking the context what it accepts, which is
    // what makes this test say nothing about which backend it is running on.
    slib::shader_library shader_lib;
    auto dxil = slib::create_dxc_compiler();
    if (dxil.has_value())
        shader_lib.add_compiler(cc::move(dxil.value()));
    auto spirv = slib::create_dxc_spirv_compiler();
    if (spirv.has_value())
        shader_lib.add_compiler(cc::move(spirv.value()));
    shader_lib.add_package(sg::test::shaders::package());

    constexpr int count = 256; // a multiple of the shader's 64-thread workgroup
    auto const out
        = ctx->persistent.create_buffer<u32>(count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(out.raw() != nullptr);

    auto disp = ctx->create_command_list();
    if (!pattern_fill_routine::is_usable(*disp))
    {
        ctx->drop_command_list(cc::move(disp));
        SKIP("no compiler reaches a shader format this context accepts");
    }

    pattern_fill_routine::execute(*disp, out);
    ctx->submit_command_list(cc::move(disp));

    // Read the result back in a second list (the buffer decays to COMMON between submits).
    auto down = ctx->create_command_list();
    auto const future = down->download.data_from_buffer<u32>(out.raw(), 0, count);
    ctx->submit_command_list(cc::move(down));

    ctx->block_until_idle();
    auto const data = future.try_get_data();
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == isize(count));
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != u32(i) * 3u + 7u)
            ok = false;
    CHECK(ok);
}

// Gated on CC_HAS_THREADS: a single-threaded build (SC_THREADS=OFF, the WASM/no-threads mode) compiles cc::mutex with no mutex member and no locking at all,
// because nothing in such a build is supposed to contend.
// Spawning std::threads there would race by construction and prove nothing about the guard.
#if CC_HAS_THREADS

// Registration races; initialization does not.
//
// This test used to race the PHASE ENGINE, because acquire ran the phases on whichever thread got there first.
// It cannot any more -- only the tick runs them.
// What is left to race is the registry: many threads asking for a routine that does not exist yet must produce one
// instance rather than eight, and the phases must still run once over it.
INVOCABLE_TEST("sg - concurrent first acquires register one instance, and the tick initializes it once",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);

    // The exclusion tag is what makes `inits == 1` meaningful: sg::reload_generation() is process-global, and a
    // concurrent sg::signal_reload() elsewhere would legitimately re-run the phases here.
    //
    // racing_routine's counters are static (see there), so clear them before the race — a prior run against another
    // backend in the same process would otherwise carry in.
    racing_routine::evict(*ctx);
    racing_routine::once = 0;
    racing_routine::inits = 0;

    constexpr auto thread_count = 8;
    auto threads = cc::vector<std::thread>::create_with_capacity(thread_count);

    cc::atomic<int> ready = 0;
    for (auto i = 0; i < thread_count; ++i)
        threads.emplace_back(
            [&]
            {
                // Line the threads up so they reach the registry together, not one after another.
                auto cmd = ctx->create_command_list();
                ++ready;
                while (ready.load() < thread_count)
                    std::this_thread::yield();
                // No CHECK here: a check on a spawned thread is not attributed to the running test, and what is being
                // proven is the aggregate below rather than anything one thread sees.
                (void)racing_routine::try_acquire(*cmd);
                ctx->drop_command_list(cc::move(cmd));
            });

    for (auto& t : threads)
        t.join();

    // Eight racing registrations, one instance, and the phases run over it exactly once.
    CHECK(racing_routine::once.load() == 0); // nothing has ticked yet
    (void)ctx->routines.tick_until_idle();
    CHECK(racing_routine::once.load() == 1);
    CHECK(racing_routine::inits.load() == 1);
}

INVOCABLE_TEST("sg - try_acquire_exclusive serializes concurrent access to a routine's own state",
               (sg::context_handle const& ctx))
{
    // Unguarded, the plain-int increment races and the total lands below the expected count.
    REQUIRE(ctx != nullptr);

    constexpr auto thread_count = 8;
    constexpr auto bumps_per_thread = 2000;

    auto probe = ctx->create_command_list();
    (void)racing_counter_routine::try_acquire(*probe);
    (void)ctx->routines.tick_until_idle();
    auto const before = racing_counter_routine::count_of(*probe);
    ctx->drop_command_list(cc::move(probe));

    auto threads = cc::vector<std::thread>::create_with_capacity(thread_count);
    cc::atomic<int> ready = 0;

    for (auto i = 0; i < thread_count; ++i)
        threads.emplace_back(
            [&]
            {
                // Each thread records against its own command list, which is the situation the guard is for.
                auto cmd = ctx->create_command_list();

                ++ready;
                while (ready.load() < thread_count)
                    std::this_thread::yield();

                for (auto n = 0; n < bumps_per_thread; ++n)
                    racing_counter_routine::bump(*cmd);

                ctx->drop_command_list(cc::move(cmd));
            });

    for (auto& t : threads)
        t.join();

    auto cmd = ctx->create_command_list();
    CHECK(racing_counter_routine::count_of(*cmd) == before + thread_count * bumps_per_thread);
    ctx->drop_command_list(cc::move(cmd));
}

#endif // CC_HAS_THREADS


// A parametrized routine is one instance per distinct parameter value, and each instance knows which value it is for.
// Serving one instance for two values would mean a pipeline built for the wrong format -- wrong output rather than
// slow output, which is why the parameter is part of the registry key rather than something execute() re-checks.
INVOCABLE_TEST("sg - a parametrized routine has one instance per parameter value", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto cmd = ctx->create_command_list();

    (void)formatted_routine::try_acquire(*cmd, sg::pixel_format::rgba8_unorm);
    (void)formatted_routine::try_acquire(*cmd, sg::pixel_format::bgra8_unorm);
    (void)ctx->routines.tick_until_idle();

    auto const rgba = formatted_routine::try_acquire(*cmd, sg::pixel_format::rgba8_unorm);
    auto const bgra = formatted_routine::try_acquire(*cmd, sg::pixel_format::bgra8_unorm);
    REQUIRE(rgba.is_ready());
    REQUIRE(bgra.is_ready());

    CHECK(&*rgba != &*bgra);
    CHECK(rgba->seen == sg::pixel_format::rgba8_unorm);
    CHECK(bgra->seen == sg::pixel_format::bgra8_unorm);
    CHECK(rgba->params() == sg::pixel_format::rgba8_unorm);

    // Asking again hands back the same instance rather than building a third.
    // The per-thread acquire memo matches on the parameter hash too, so alternating between two values must not
    // keep serving whichever was cached last.
    CHECK(&*formatted_routine::try_acquire(*cmd, sg::pixel_format::rgba8_unorm) == &*rgba);
    CHECK(&*formatted_routine::try_acquire(*cmd, sg::pixel_format::bgra8_unorm) == &*bgra);
    CHECK(rgba->inits == 1);
    CHECK(bgra->inits == 1);

    // evict() takes the parameter, so it drops one instance and leaves the other alone.
    formatted_routine::evict(*ctx, sg::pixel_format::rgba8_unorm);
    CHECK(formatted_routine::try_acquire(*cmd, sg::pixel_format::bgra8_unorm)->inits == 1);

    // The evicted one is gone, so asking registers a fresh instance that the next tick builds from scratch.
    CHECK(formatted_routine::try_acquire(*cmd, sg::pixel_format::rgba8_unorm).is_pending());
    (void)ctx->routines.tick_until_idle();
    CHECK(formatted_routine::try_acquire(*cmd, sg::pixel_format::rgba8_unorm)->inits == 1);

    formatted_routine::evict_all(*ctx);
    ctx->drop_command_list(cc::move(cmd));
}


// prewarm registers a routine; the TICK is what brings it up.
// Those were the same call before, so a caller who still expects prewarm to initialize now gets a routine that is
// registered and not ready -- which is what the tick's diagnostic is for, once acquire stops initializing on demand.
//
// Exclusive on the same tag as the phase-counting tests: a tick initializes EVERY registered routine in the context,
// so it would otherwise land in the middle of another test's counts.
INVOCABLE_TEST("sg - prewarm registers a routine and the tick brings it up",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);

    using prewarmed = slow_routine<0>;
    prewarmed::evict(*ctx);

    prewarmed::prewarm(*ctx);

    auto const first = ctx->routines.tick();
    CHECK(first.initialized >= 1);

    // Everything registered is up, so a second tick has nothing to do and says so.
    auto const second = ctx->routines.tick();
    CHECK(second.initialized == 0);
    CHECK(second.is_idle());

    auto cmd = ctx->create_command_list();
    auto const up = prewarmed::try_acquire(*cmd);
    REQUIRE(up.is_ready());
    CHECK(up->ran);
    ctx->drop_command_list(cc::move(cmd));

    prewarmed::evict(*ctx);
}

// The budget bounds how long the TICK spends driving, not how long an initialization takes.
// A phase runs on a worker, so a budget below what the phases cost returns with them still in flight -- and a later
// tick collects them.
// Pacing rather than a deadline, which is why this asserts on what was left rather than on elapsed time.
INVOCABLE_TEST("sg - a tick stops at its budget and leaves the rest pending",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);

    using first_slow = slow_routine<1>;
    using second_slow = slow_routine<2>;

    // Bring everything else up, so the only pending routines are the two below.
    (void)ctx->routines.tick_until_idle();

    first_slow::evict(*ctx);
    second_slow::evict(*ctx);
    first_slow::prewarm(*ctx);
    second_slow::prewarm(*ctx);

    // A budget far below what the two routines cost, so the tick returns before either has settled.
    auto const bounded = ctx->routines.tick({.budget_secs = 0.001});
    CHECK(bounded.budget_exhausted);
    CHECK(!bounded.is_idle());

    // The work the bounded tick started is still running; this is what collects it.
    auto const rest = ctx->routines.tick_until_idle();
    CHECK(rest.is_idle());
    CHECK(first_slow::try_acquire(*ctx)->ran);
    CHECK(second_slow::try_acquire(*ctx)->ran);

    first_slow::evict(*ctx);
    second_slow::evict(*ctx);
}

// try_acquire REPORTS; it never initializes.
// A routine nothing has ticked reads as pending rather than quietly bringing itself up on the frame path, which is the
// whole difference between the two entry points.
INVOCABLE_TEST("sg - try_acquire reports readiness without initializing",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);

    using reported = slow_routine<3>;
    reported::evict(*ctx);

    // Asking is enough to register it, so the next tick brings it up -- but asking did not bring it up.
    auto const pending = reported::try_acquire(*ctx);
    CHECK(pending.is_pending());
    CHECK(!pending.is_ready());
    CHECK(!pending.is_failed());

    (void)ctx->routines.tick_until_idle();

    auto const ready = reported::try_acquire(*ctx);
    CHECK(ready.is_ready());
    CHECK(ready->ran);

    // The exclusive form reports the same thing, and holds the lock while doing it.
    auto guard = reported::try_acquire_exclusive(*ctx);
    CHECK(guard.is_ready());
    CHECK(guard->ran);

    reported::evict(*ctx);
}

namespace
{
// A dependency chain: chained_leaf <- chained_middle <- chained_top.
// The token is minted during init and redeemed through the scope, which is what makes "valid while the holder is
// acquired" a compile-time property rather than a runtime check.
class chained_leaf : public sg::render_routine<chained_leaf>
{
public:
    int value = 0;

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope) override
    {
        value = 7;
        co_return;
    }
};

class chained_middle : public sg::render_routine<chained_middle>
{
public:
    [[nodiscard]] int leaf_value(sg::routine_scope<chained_middle> const& self) const
    {
        return self.acquire(_leaf).value;
    }

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override
    {
        _leaf = depend_on<chained_leaf>(scope.context());
        co_return;
    }

private:
    sg::routine_dependency<chained_leaf, sg::routine_no_params> _leaf;
};

class chained_top : public sg::render_routine<chained_top>
{
protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override
    {
        _middle = depend_on<chained_middle>(scope.context());
        co_return;
    }

private:
    sg::routine_dependency<chained_middle, sg::routine_no_params> _middle;
};
} // namespace

// A holder is not ready until its whole subtree is, which is what makes redeeming a token inside it infallible.
// Without the fold, `top` would report ready while the leaf it transitively needs had never run.
INVOCABLE_TEST("sg - a routine is not ready until its dependencies are",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    chained_top::evict(*ctx);
    chained_middle::evict(*ctx);
    chained_leaf::evict(*ctx);

    // Registers top; its init has not run, so the edges do not exist yet either.
    CHECK(chained_top::try_acquire(*ctx).is_pending());

    (void)ctx->routines.tick_until_idle();

    auto const top = chained_top::try_acquire(*ctx);
    CHECK(top.is_ready());

    // The middle reaches the leaf through the token, never by acquiring it again.
    auto const middle = chained_middle::try_acquire(*ctx);
    REQUIRE(middle.is_ready());
    CHECK(middle->leaf_value(middle) == 7);

    chained_top::evict(*ctx);
    chained_middle::evict(*ctx);
    chained_leaf::evict(*ctx);
}

namespace
{
// Two routines that need each other.
// Declared apart and defined below, because each init names the other's type.
class cyclic_b;

class cyclic_a : public sg::render_routine<cyclic_a>
{
protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::routine_dependency<cyclic_b, sg::routine_no_params> _b;
};

class cyclic_b : public sg::render_routine<cyclic_b>
{
protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::routine_dependency<cyclic_a, sg::routine_no_params> _a;
};

cc::shared_async<cc::unit> cyclic_a::init(sg::routine_init_scope scope)
{
    _b = depend_on<cyclic_b>(scope.context());
    co_return;
}
cc::shared_async<cc::unit> cyclic_b::init(sg::routine_init_scope scope)
{
    _a = depend_on<cyclic_a>(scope.context());
    co_return;
}

/// What the test's assertion handler throws to unwind out of the phase that closed the cycle.
struct cycle_assert
{
};
} // namespace

// A cycle is refused where the edge is declared, not discovered later as a hang.
// It is two failures at once: readiness never settles because each end waits for the other, and initialization would
// deadlock taking the two routines' locks in opposite orders -- a stack trace naming two mutexes and no routine.
//
// CHECK_ASSERTS cannot be used here, and the reason is structural: it works by throwing out of the expression, and the
// assert fires inside a PHASE, whose promise catches the exception and fails the node instead of unwinding to the tick.
// So the handler is installed by hand, and what is checked is both halves -- that the assert fired, and that the
// framework then reports the cycle as a failed routine rather than one pending forever.
//
// Singlethreaded for a reason of the same kind: the handler stack is per-thread, so the phases have to run inline on
// this thread rather than on a pool worker that nobody scoped.
INVOCABLE_TEST("sg - a dependency cycle is refused where it is declared",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"),
               singlethreaded)
{
    REQUIRE(ctx != nullptr);
    cyclic_a::evict(*ctx);
    cyclic_b::evict(*ctx);

#if CC_ASSERT_ENABLED
    auto asserts_seen = 0;
    {
        auto const handler = cc::impl::scoped_assertion_handler(
            [&](cc::impl::assertion_info const&)
            {
                ++asserts_seen;
                throw cycle_assert{}; // unwinds out of the phase, exactly as nexus's own handler would
            });

        cyclic_a::prewarm(*ctx);
        (void)ctx->routines.tick_until_idle();
    }
    CHECK(asserts_seen == 1);

    // Both ends are unusable: the one that closed the edge failed outright, and the one above it inherits that
    // through the subtree fold rather than reporting ready over a dependency that is not coming.
    CHECK(cyclic_b::try_acquire(*ctx).is_failed());
    CHECK(cyclic_a::try_acquire(*ctx).is_failed());
#else
    // With assertions off the cycle is not refused at all, and both ends come up over an edge that loops.
    // What still has to hold is that nothing hangs: the readiness walk carries a visited set, so a cycle costs a leak
    // rather than a spin.
    cyclic_a::prewarm(*ctx);
    (void)ctx->routines.tick_until_idle();
    CHECK(cyclic_a::try_acquire(*ctx).is_ready());
#endif

    // The registry is left holding the half-built graph, so clear it rather than leaving it for the next test.
    cyclic_a::evict(*ctx);
    cyclic_b::evict(*ctx);
}

namespace
{
// A routine whose init cannot succeed.
// It stands in for a shader that was never good — the first compile failing, or no compiler reaching a format this
// context accepts.
// A failed RELOAD is not this: slib keeps the last shader that compiled.
class broken_routine : public sg::render_routine<broken_routine>
{
protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope) override
    {
        fail_init();
        co_return;
    }
};
} // namespace

// "Still compiling" and "will never compile" have to be different answers.
// Collapsed into one, a routine with a broken shader reads as pending forever: every caller keeps skipping it, no
// frame ever looks wrong enough to investigate, and nothing anywhere says why.
INVOCABLE_TEST("sg - a routine whose init fails reports failed, not pending",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    broken_routine::evict(*ctx);

    CHECK(broken_routine::try_acquire(*ctx).is_pending()); // registered, not yet attempted

    (void)ctx->routines.tick_until_idle();

    auto const failed = broken_routine::try_acquire(*ctx);
    CHECK(failed.is_failed());
    CHECK(!failed.is_ready());
    CHECK(!failed.is_pending());

    // A reload is a fresh verdict: the phases run again and get another chance to compile.
    // (This one fails again, so it is the re-attempt that is being checked, not the outcome.)
    sg::signal_reload();
    (void)ctx->routines.tick_until_idle();
    CHECK(broken_routine::try_acquire(*ctx).is_failed());

    broken_routine::evict(*ctx);
}
