#pragma once

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// Shared helper for the dx12 backend test binary (shaped-graphics-dx12-test).
// The tests are split per topic across several .cc files; this is the one piece they all reuse.
// Header-only.
//
// Contexts are handed around as dx12_context_handle rather than sg::context_handle, so a test that inspects backend guts needs no downcast.
// Driving still goes through the abstract API — see libs/graphics/shaped-graphics/docs/testing.md.
//
// Most tests are INVOCABLE_TESTs taking the context the entry driver (dx12-entry.cc) built — one per adapter, for the whole run.
// The helpers here are for the few that need a context of their own: pristine epoch/pool state, or a backend knob the test is about.

namespace sg::backend::dx12
{
struct scoped_expected_validation_messages;
} // namespace sg::backend::dx12

namespace sg::backend::dx12
{
/// Debug-layer advisories sg provokes on purpose, matched as substrings.
/// Each entry is a decision, not a mute button: the message is understood, and the alternative is worse or does not exist yet.
/// Anything not listed fails the test, so a NEW warning is still loud.
inline constexpr cc::string_view k_expected_validation_messages[] = {
    // A perf advisory: D3D12 wants an optimized clear value at texture creation, matching what the target is later cleared to.
    // sg::texture_description has no such field, and inventing one that disagrees with the actual clear is worse than none.
    "did not pass any clear value to resource creation",

    // A command list carrying only barriers is a legitimate sg shape — a list opened purely to transition resources.
    "recorded only Barrier commands",
};

/// Set while a test is deliberately provoking a validation message; see scoped_expected_validation_messages.
inline thread_local bool tl_expect_validation_messages = false;

} // namespace sg::backend::dx12

/// Suppresses the listener below for the calling thread, for a test whose subject IS the bad input.
///
/// Thread-scoped rather than per-context, because D3D12 hands one debug-layer message to EVERY callback registered in the process, not only the one on the device that raised it.
/// With several contexts alive — which is the normal state of this suite at -jN — clearing one context's listener leaves the other N-1 to fail the test.
/// The message is raised synchronously on the thread that provoked it, so the thread is what names the right test.
struct sg::backend::dx12::scoped_expected_validation_messages
{
    scoped_expected_validation_messages() { tl_expect_validation_messages = true; }
    ~scoped_expected_validation_messages() { tl_expect_validation_messages = false; }

    scoped_expected_validation_messages(scoped_expected_validation_messages const&) = delete;
    scoped_expected_validation_messages& operator=(scoped_expected_validation_messages const&) = delete;
};

namespace sg::backend::dx12
{

/// Fails whichever test provoked it on any debug-layer message of warning severity or worse, bar the expected ones above.
/// Without this a validation error is a line on stderr nobody reads, and the run stays green.
/// Attribution rides the ambient context, so the check lands on the right test wherever the runtime raised the message.
inline void fail_on_validation_messages(dx12_context_handle const& ctx)
{
    ctx->set_message_callback(
        [](dx12_message_severity severity, cc::string_view message)
        {
            if (severity > dx12_message_severity::warning || tl_expect_validation_messages)
                return;
            for (auto const expected : k_expected_validation_messages)
                if (message.contains(expected))
                    return;

            CHECK(false).context(cc::format("dx12 debug layer: {}", message));
        });
}

/// The backend-typed view of a freshly created context, with the validation listener installed.
/// Passes an error through untouched, so a caller can SKIP.
inline cc::result<dx12_context_handle> as_test_context(cc::result<sg::context_handle> ctx)
{
    if (ctx.has_error())
    {
        // The reason, not just the fact: a caller's `REQUIRE(ctx.has_value())` reports neither the HRESULT nor which step failed, and creation is the step that breaks under contention.
        cc::eprintln("[dx12-test] context creation failed: {}", ctx.error().to_string());
        return cc::error(cc::move(ctx).error());
    }

    auto typed = std::static_pointer_cast<dx12_context>(ctx.value());
    fail_on_validation_messages(typed);
    return typed;
}

/// A context for a test to own: WARP, debug layer on, validation messages failing the test.
/// `config` supplies the backend knobs the test is about — the two fields above are set here regardless.
/// Errors on the rare host without WARP, so a caller can SKIP.
inline cc::result<dx12_context_handle> make_test_context(dx12_config config = {})
{
    config.use_warp = true;
    config.enable_debug_layer = true;
    return as_test_context(sg::create_dx12_context(config));
}

/// make_test_context as a bare handle, for the tests that assert pristine state — the epoch counter, allocator/list pool counts — and need no knobs.
/// nullptr when it could not be created; as_test_context has already said why.
inline dx12_context_handle make_warp_context()
{
    auto ctx = make_test_context();
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace sg::backend::dx12
