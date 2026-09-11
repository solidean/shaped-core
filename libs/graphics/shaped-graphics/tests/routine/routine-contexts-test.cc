#include <clean-core/common/utility.hh> // cc::move
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/routine/reload_generation.hh>
#include <shaped-graphics/routine/render_routine.hh>

// The two routine tests that need TWO live contexts, which is why they are not in routine-test.cc.
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
    int declare = 0;
    int materialize = 0;

protected:
    void init_once(sg::context&) override { ++once; }
    void init_declare(sg::context&) override { ++declare; }
    void init_materialize(sg::command_list&) override { ++materialize; }
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
        auto const& ra = counting_routine::acquire(*cmd_a);
        CHECK(ra.once == 1);
        CHECK(ra.declare == 1);
        CHECK(ra.materialize == 1);
        ctx_a->drop_command_list(cc::move(cmd_a));
    } // ctx_a shuts down here — its routine instance (and cached GPU state) is released with it.

    // Advance the global generation.
    // On A's instance this would only bump declare/materialize; a fresh context must instead build its OWN instance from scratch — init_once included.
    sg::signal_reload();

    auto const ctx_b = make_warp_context();
    REQUIRE(ctx_b != nullptr);

    auto cmd_b = ctx_b->create_command_list();
    auto const& rb = counting_routine::acquire(*cmd_b);
    CHECK(rb.once == 1); // ran again on ctx_b: the instance is per-context, not a process singleton
    CHECK(rb.declare == 1);
    CHECK(rb.materialize == 1);
    ctx_b->drop_command_list(cc::move(cmd_b));
}

TEST("sg - two live contexts keep separate routine instances")
{
    auto const ctx_a = make_warp_context();
    if (ctx_a == nullptr)
        SKIP("no dx12 WARP device");
    auto const ctx_b = make_warp_context();
    REQUIRE(ctx_b != nullptr);

    auto cmd_a = ctx_a->create_command_list();
    auto cmd_b = ctx_b->create_command_list();

    auto const& ra = counting_routine::acquire(*cmd_a);
    auto const& rb = counting_routine::acquire(*cmd_b);
    CHECK(&ra != &rb);

    // Interleaved acquires must keep landing on the right instance — neither context may be served the other's routine, however the per-thread acquire cache ping-pongs between them.
    (void)counting_routine::acquire(*cmd_a);
    (void)counting_routine::acquire(*cmd_b);
    (void)counting_routine::acquire(*cmd_a);

    CHECK(ra.once == 1);
    CHECK(ra.declare == 1);
    CHECK(rb.once == 1);
    CHECK(rb.declare == 1);

    ctx_a->drop_command_list(cc::move(cmd_a));
    ctx_b->drop_command_list(cc::move(cmd_b));
}
