#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
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
//   1. the framework's phase orchestration re-runs declare/materialize on a reload, but not init_once,
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
    int declare = 0;
    int materialize = 0;

protected:
    void init_once(sg::context&) override { ++once; }
    void init_declare(sg::context&) override { ++declare; }
    void init_materialize(sg::command_list&) override { ++materialize; }
};

using phases_routine = counting_routine<0>;
using evict_routine = counting_routine<1>;

// Like counting_routine, but counted atomically so racing acquires can be checked.
// The counters are static so the test can read them after the race without a handle to the per-context instance.
class racing_routine : public sg::render_routine<racing_routine>
{
public:
    static inline cc::atomic<int> once = 0;
    static inline cc::atomic<int> declare = 0;

protected:
    void init_once(sg::context&) override { ++once; }
    // The sleep widens the window a racing second caller would slip through, so the test below actually exercises the lock instead of passing because the first thread happened to finish first.
    // Only the winner ever sleeps, so it costs one interval, not one per thread.
    void init_declare(sg::context&) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ++declare;
    }
};

// Counts increments made through acquire_exclusive, deliberately with a PLAIN int rather than an atomic.
// The guard is what must make the read-modify-write exclusive, and an atomic would pass with or without it.
template <int Tag>
class counter_routine : public sg::render_routine<counter_routine<Tag>>
{
public:
    static void bump(sg::command_list& cmd)
    {
        auto self = counter_routine::acquire_exclusive(cmd);
        ++self->_count;
    }

    [[nodiscard]] static int count_of(sg::command_list& cmd) { return counter_routine::acquire_exclusive(cmd)->_count; }

    // Reads the same member through the unlocked path, which only compiles because it does not mutate.
    [[nodiscard]] static int count_via_const(sg::command_list& cmd) { return counter_routine::acquire(cmd)._count; }

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
    int declare = 0;
    sg::pixel_format seen = sg::pixel_format::undefined;

protected:
    void init_declare(sg::context&) override
    {
        ++declare;
        seen = params();
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
    void init_declare(sg::context&) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ran = true;
    }
};

// The end-to-end routine: owns its pipeline via init_declare, dispatches in execute.
// Reached by type — no handle, no registration call.
class pattern_fill_routine : public sg::render_routine<pattern_fill_routine>
{
public:
    static void execute(sg::command_list& cmd, sg::buffer<u32> const& out)
    {
        auto const& self = acquire(cmd);
        CC_ASSERT(self._pipeline != nullptr, "pattern_fill routine failed to initialize");

        // Force the compute pipeline only now — init_declare merely kicked off the background compile.
        auto const pipeline = cc::async_blocking_get(self._pipeline);

        auto const group = cmd.context().transient.create_binding_group(
            self._group_layout, {{.name = "gValues", .view = out.as_readwrite_buffer()}});

        cmd.compute.bind_pipeline(*pipeline);
        cmd.compute.bind_group(0, *group);
        cmd.compute.dispatch_threads(out.element_count());
    }

    [[nodiscard]] static bool is_usable(sg::command_list& cmd) { return acquire(cmd)._pipeline != nullptr; }

protected:
    void init_declare(sg::context& ctx) override
    {
        auto const shader = sg::test::shaders::pattern_fill.compute.main->acquire(ctx);
        (void)cc::try_async_blocking_get(shader); // no async pool here, so drive it inline
        auto const* const compiled = shader->try_value();
        if (compiled == nullptr)
            return; // the context cannot produce a format we can use; is_usable() then reports it

        _group_layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
        auto const layout
            = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {_group_layout}});
        // Only kick off the background compile here — execute() forces it when it actually needs the pipeline.
        _pipeline = ctx.cached.acquire_compute_pipeline(
            sg::compute_pipeline_description{.shader = *compiled, .layout = layout});
    }

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};
} // namespace

INVOCABLE_TEST("sg - routine acquire hands out a const reference, acquire_exclusive a move-only guard",
               (sg::context_handle const& ctx))
{
    // A mutable reference creeping back into the unlocked path would silently re-open unguarded writes.
    static_assert(
        std::is_same_v<decltype(guard_routine::acquire(std::declval<sg::command_list&>())), guard_routine const&>);
    static_assert(std::is_same_v<decltype(guard_routine::acquire_exclusive(std::declval<sg::command_list&>())),
                                 sg::routine_guard<guard_routine>>);
    static_assert(!std::is_copy_constructible_v<sg::routine_guard<guard_routine>>);

    REQUIRE(ctx != nullptr);
    auto cmd = ctx->create_command_list();

    // Both entry points reach the one per-context instance — a write through the guard is what the const path then reads.
    auto const before = guard_routine::count_of(*cmd);
    guard_routine::bump(*cmd);
    CHECK(guard_routine::count_of(*cmd) == before + 1);
    CHECK(guard_routine::count_via_const(*cmd) == before + 1);

    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg - routine phases run once, then re-run declare + materialize on a reload",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto cmd = ctx->create_command_list();

    auto const& routine = phases_routine::acquire(*cmd);
    CHECK(routine.once == 1);
    CHECK(routine.declare == 1);
    CHECK(routine.materialize == 1);

    // A second pass at the same generation changes nothing (same per-context instance).
    // We re-read through `routine`, so the returned reference is intentionally discarded.
    (void)phases_routine::acquire(*cmd);
    CHECK(routine.once == 1);
    CHECK(routine.declare == 1);
    CHECK(routine.materialize == 1);

    // A reload bumps the global generation: declare + materialize re-run, init_once does not.
    sg::signal_reload();
    (void)phases_routine::acquire(*cmd);
    CHECK(routine.once == 1);
    CHECK(routine.declare == 2);
    CHECK(routine.materialize == 2);

    ctx->drop_command_list(cc::move(cmd));
}

INVOCABLE_TEST("sg - evicting a routine drops its instance (the acquire cache does not resurrect it)",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto cmd = ctx->create_command_list();

    auto const& first = evict_routine::acquire(*cmd);
    CHECK(first.once == 1);

    // Drive the first instance's declare count to 2, so it is distinguishable from a fresh one.
    sg::signal_reload();
    (void)evict_routine::acquire(*cmd);
    CHECK(first.declare == 2);

    evict_routine::evict(*ctx);

    // A fresh instance, built from scratch: every phase back at 1.
    // A cached slot that survived the eviction would instead hand back the old object (declare == 2) — or worse, a freed one.
    auto const& second = evict_routine::acquire(*cmd);
    CHECK(second.once == 1);
    CHECK(second.declare == 1);
    CHECK(second.materialize == 1);

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

    auto const data = ctx->wait_for(future);
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

INVOCABLE_TEST("sg - concurrent acquires of one routine run each phase exactly once",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    // The phase engine is guarded, so racing acquires must not both run init_declare.
    // Without that lock this is a plain data race on the phase flags, and the counts come out above one under contention.
    REQUIRE(ctx != nullptr);

    // The exclusion tag is what makes `declare == 1` meaningful: sg::reload_generation() is process-global, and a concurrent
    // sg::signal_reload() elsewhere would legitimately re-run init_declare here.
    //
    // racing_routine's counters are static (see there), so clear them before the race — a prior run against another
    // backend in the same process would otherwise carry in.
    racing_routine::evict(*ctx);
    racing_routine::once = 0;
    racing_routine::declare = 0;

    constexpr auto thread_count = 8;
    auto threads = cc::vector<std::thread>::create_with_capacity(thread_count);

    cc::atomic<int> ready = 0;
    for (auto i = 0; i < thread_count; ++i)
        threads.emplace_back(
            [&]
            {
                // Line the threads up so they hit the phase engine together, not one after another.
                auto cmd = ctx->create_command_list();
                ++ready;
                while (ready.load() < thread_count)
                    std::this_thread::yield();
                (void)racing_routine::acquire(*cmd);
                ctx->drop_command_list(cc::move(cmd));
            });

    for (auto& t : threads)
        t.join();

    CHECK(racing_routine::once.load() == 1);
    CHECK(racing_routine::declare.load() == 1);
}

INVOCABLE_TEST("sg - acquire_exclusive serializes concurrent access to a routine's own state",
               (sg::context_handle const& ctx))
{
    // Unguarded, the plain-int increment races and the total lands below the expected count.
    REQUIRE(ctx != nullptr);

    constexpr auto thread_count = 8;
    constexpr auto bumps_per_thread = 2000;

    auto probe = ctx->create_command_list();
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

    auto const& rgba = formatted_routine::acquire(*cmd, sg::pixel_format::rgba8_unorm);
    auto const& bgra = formatted_routine::acquire(*cmd, sg::pixel_format::bgra8_unorm);

    CHECK(&rgba != &bgra);
    CHECK(rgba.seen == sg::pixel_format::rgba8_unorm);
    CHECK(bgra.seen == sg::pixel_format::bgra8_unorm);
    CHECK(rgba.params() == sg::pixel_format::rgba8_unorm);

    // Re-acquiring either one hands back the same instance rather than building a third.
    // The per-thread acquire memo matches on the parameter hash too, so alternating between two values must not
    // keep serving whichever was cached last.
    CHECK(&formatted_routine::acquire(*cmd, sg::pixel_format::rgba8_unorm) == &rgba);
    CHECK(&formatted_routine::acquire(*cmd, sg::pixel_format::bgra8_unorm) == &bgra);
    CHECK(rgba.declare == 1);
    CHECK(bgra.declare == 1);

    // evict() takes the parameter, so it drops one instance and leaves the other alone.
    formatted_routine::evict(*ctx, sg::pixel_format::rgba8_unorm);
    CHECK(formatted_routine::acquire(*cmd, sg::pixel_format::bgra8_unorm).declare == 1);
    CHECK(formatted_routine::acquire(*cmd, sg::pixel_format::rgba8_unorm).declare == 1); // a fresh one, back at 1

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
    CHECK(prewarmed::acquire(*cmd).ran);
    ctx->drop_command_list(cc::move(cmd));

    prewarmed::evict(*ctx);
}

// The budget is checked between routines, so a tick that has two slow ones to bring up stops after the first.
// It is pacing rather than a deadline: the tick still overruns by however long one routine takes, which is why this
// asserts on what was left rather than on elapsed time.
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

    // A budget far below what one routine costs, so the check between routines always trips after the first.
    auto const bounded = ctx->routines.tick({.budget_secs = 0.001});
    CHECK(bounded.initialized == 1);
    CHECK(bounded.pending == 1);
    CHECK(bounded.budget_exhausted);

    auto const rest = ctx->routines.tick_until_idle();
    CHECK(rest.initialized == 1);
    CHECK(rest.is_idle());

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
    void init_declare(sg::context&) override { value = 7; }
};

class chained_middle : public sg::render_routine<chained_middle>
{
public:
    [[nodiscard]] int leaf_value(sg::routine_scope<chained_middle> const& self) const
    {
        return self.acquire(_leaf).value;
    }

protected:
    void init_declare(sg::context& ctx) override { _leaf = depend_on<chained_leaf>(ctx); }

private:
    sg::routine_dependency<chained_leaf, sg::routine_no_params> _leaf;
};

class chained_top : public sg::render_routine<chained_top>
{
protected:
    void init_declare(sg::context& ctx) override { _middle = depend_on<chained_middle>(ctx); }

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
    void init_declare(sg::context& ctx) override;

private:
    sg::routine_dependency<cyclic_b, sg::routine_no_params> _b;
};

class cyclic_b : public sg::render_routine<cyclic_b>
{
protected:
    void init_declare(sg::context& ctx) override;

private:
    sg::routine_dependency<cyclic_a, sg::routine_no_params> _a;
};

void cyclic_a::init_declare(sg::context& ctx)
{
    _b = depend_on<cyclic_b>(ctx);
}
void cyclic_b::init_declare(sg::context& ctx)
{
    _a = depend_on<cyclic_a>(ctx);
}
} // namespace

// A cycle is refused where the edge is declared, not discovered later as a hang.
// It is two failures at once: readiness never settles because each end waits for the other, and initialization would
// deadlock taking the two routines' locks in opposite orders -- a stack trace naming two mutexes and no routine.
INVOCABLE_TEST("sg - a dependency cycle is refused where it is declared",
               (sg::context_handle const& ctx),
               exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    cyclic_a::evict(*ctx);
    cyclic_b::evict(*ctx);

    cyclic_a::prewarm(*ctx);
    CHECK_ASSERTS(ctx->routines.tick_until_idle());

    // The registry is left holding the half-built graph, so clear it rather than leaving it for the next test.
    cyclic_a::evict(*ctx);
    cyclic_b::evict(*ctx);
}
