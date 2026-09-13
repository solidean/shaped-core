#include <clean-core/common/assert-handler.hh>
#include <clean-core/common/macros.hh>  // CC_ASSERT_ENABLED
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/routine/reload_generation.hh>
#include <shaped-graphics/routine/render_routine.hh>

// The routine tests that must create their own context, which is why they are not in routine-test.cc.
// Two of them need TWO live contexts; the cycle test needs `singlethreaded`, which an invocable cannot have.
//
// The backend-agnostic harness hands an invocable ONE context (see tests/backends/vulkan-entry.cc), so a test that
// compares two of them has to create its own — and creating one means naming a backend.
// dx12 WARP is the one that is always there on a Windows host, hence this file's gate.
// Making these portable would mean teaching the harness to hand out a context FACTORY; worth doing if a second
// registry question ever needs it, not for two tests.

namespace
{
// Records how often each phase ran; does no GPU work.
class counting_routine : public sg::render_routine<counting_routine>
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

// A dx12 WARP context, or nullptr where none is available (the caller SKIPs).
sg::context_handle make_warp_context()
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace

TEST("sg - routines are per-context: each context builds its own instance from scratch",
     exclusive("sg-reload-generation"))
{
    // Context A initializes the routine, then goes away.
    {
        auto const ctx_a = make_warp_context();
        if (ctx_a == nullptr)
            SKIP("no dx12 WARP device");

        auto cmd_a = ctx_a->create_command_list();
        // Asking registers it; the tick is what runs the phases.
        (void)counting_routine::try_acquire(*cmd_a);
        (void)ctx_a->routines.tick_until_idle();
        auto const ra = counting_routine::try_acquire(*cmd_a);
        REQUIRE(ra.is_ready());
        CHECK(ra->once == 1);
        CHECK(ra->inits == 1);
        ctx_a->drop_command_list(cc::move(cmd_a));
    } // ctx_a shuts down here — its routine instance (and cached GPU state) is released with it.

    // Advance the global generation.
    // On A's instance this would only bump inits; a fresh context must instead build its OWN instance from scratch — init_once included.
    sg::signal_reload();

    auto const ctx_b = make_warp_context();
    REQUIRE(ctx_b != nullptr);

    auto cmd_b = ctx_b->create_command_list();
    (void)counting_routine::try_acquire(*cmd_b);
    (void)ctx_b->routines.tick_until_idle();
    auto const rb = counting_routine::try_acquire(*cmd_b);
    REQUIRE(rb.is_ready());
    CHECK(rb->once == 1); // ran again on ctx_b: the instance is per-context, not a process singleton
    CHECK(rb->inits == 1);
    ctx_b->drop_command_list(cc::move(cmd_b));
}

TEST("sg - two live contexts keep separate routine instances", exclusive("sg-reload-generation"))
{
    auto const ctx_a = make_warp_context();
    if (ctx_a == nullptr)
        SKIP("no dx12 WARP device");
    auto const ctx_b = make_warp_context();
    REQUIRE(ctx_b != nullptr);

    auto cmd_a = ctx_a->create_command_list();
    auto cmd_b = ctx_b->create_command_list();

    (void)counting_routine::try_acquire(*cmd_a);
    (void)counting_routine::try_acquire(*cmd_b);
    (void)ctx_a->routines.tick_until_idle();
    (void)ctx_b->routines.tick_until_idle();

    auto const ra = counting_routine::try_acquire(*cmd_a);
    auto const rb = counting_routine::try_acquire(*cmd_b);
    REQUIRE(ra.is_ready());
    REQUIRE(rb.is_ready());
    CHECK(&*ra != &*rb);

    // Interleaved acquires must keep landing on the right instance — neither context may be served the other's routine, however the per-thread acquire cache ping-pongs between them.
    (void)counting_routine::try_acquire(*cmd_a);
    (void)counting_routine::try_acquire(*cmd_b);
    (void)counting_routine::try_acquire(*cmd_a);

    CHECK(ra->once == 1);
    CHECK(ra->inits == 1);
    CHECK(rb->once == 1);
    CHECK(rb->inits == 1);

    ctx_a->drop_command_list(cc::move(cmd_a));
    ctx_b->drop_command_list(cc::move(cmd_b));
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
// That is also why this is a plain TEST on its own context rather than an invocable: a child dispatched by
// nx::invoke_tests runs under its driver's scheduler, so `singlethreaded` and `exclusive` on it would be ignored.
TEST("sg - a dependency cycle is refused where it is declared", exclusive("sg-reload-generation"), singlethreaded)
{
    auto const ctx = make_warp_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");

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

    // The registry is left holding the half-built graph, which links the two ends to each other, so clear it before the
    // context shuts down.
    cyclic_a::evict(*ctx);
    cyclic_b::evict(*ctx);
}
