#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

namespace sg
{
/// Configuration facade for a context's inline download path, reached as `ctx.download`.
///
/// Inline downloads themselves are recorded through `cmd.download` on a command list; this facade only
/// exposes context-wide settings for the readback ring those downloads stage through. A thin facade over
/// its owning context: it forwards each setting to the context's backend impl.
class context_download_scope
{
public:
    /// Sets the inline readback ring capacity in bytes (> 0) — the ring all `cmd.download` readbacks stage
    /// through, bounding the in-flight inline-download volume. May be called any time, repeatedly: it
    /// records a *pending* budget and returns immediately without touching the GPU. The change takes effect
    /// at the next advance_epoch, which drains outstanding readbacks and reallocates the ring; until then
    /// the current budget stays in force. Backends without an inline-download path ignore it.
    void set_budget(cc::isize bytes);

    // Pinned to its owning context: neither copyable nor movable.
    context_download_scope(context_download_scope const&) = delete;
    context_download_scope(context_download_scope&&) = delete;
    context_download_scope& operator=(context_download_scope const&) = delete;
    context_download_scope& operator=(context_download_scope&&) = delete;

private:
    // Only a context constructs its own scope; the scope in turn reaches the context's protected backend
    // virtual (mutual friendship).
    friend class context;
    explicit context_download_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;
};
} // namespace sg
