# shaped-viewer coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read that first; everything there still applies.
shaped-viewer sits on top of the graphics stack, so the [shaped-graphics coding-guidelines](../../shaped-graphics/docs/coding-guidelines.md) are relevant background too.
Those cover handles, the resource model and the backend model.

This document is intentionally near-empty for now.
**Extend it as we go:** whenever you catch yourself making a "style mistake" by following generic advice that turns out to be wrong for sv, that is the signal to add the rule here.
The bar is a reason that is not obvious from the code.

## GPU tests share their devices and run on any adapter

sv's tests follow sg's [Devices and adapters](../../shaped-graphics/docs/testing.md#devices-and-adapters) rules, and they are rules rather than advice.
[tests/dx12-entry.cc](../tests/dx12-entry.cc) brings up one context per adapter and invokes every `INVOCABLE_TEST("sv - …", (sg::context_handle const& ctx_h))` against it.
So a new GPU test is an invocable by default, and `dev.py test "sv - <name>"` still selects it alone, on both adapters.
The invocables run one after another on that context, so a test leaves nothing behind: no open command list, and no compile it started still running.
A test creates its own context only when the context is its subject, as `sv::set_acquire_context` is.
The hardware adapter is the default, WARP runs only where there is none or under `--thorough`, and a test passes on either.
