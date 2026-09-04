# Waiting on an async that a pump has to drive

**Status:** two bugs and one convenience, none of them cnet's.
They surfaced reviewing clean-net's examples, and every one of them is in clean-core or nexus.

The examples in [libs/base/clean-net/examples/](../../libs/base/clean-net/examples/) each carry a hand-rolled `await`:

```cpp
template <class T>
void await(cc::shared_async<T> const& a)
{
    while (!a->is_ready())
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
}
```

The obvious question is why that is not `cc::async_blocking_get`, and the answer today is that
`cc::async_blocking_get` **hangs**.

## What actually happens

`cc::impl::async_drive_until_ready` does sweep the pump registry, and it does it second:

```cpp
while (!root.is_ready())
{
    scheduler.participate_until_ready(root);   // parks here
    if (root.is_ready())
        return;

    if (cc::thread_pump_all())
        continue;
    ...
}
```

`participate_until_ready` parks in a pool slot until the root is ready — `async_thread_pool.hh` says so, and
`async_drive_until_ready_for` says it again as its reason for not calling it.
An unthreaded `cnet::io_system` completes its operations from a **pump**, which only the line below ever sweeps.
So the pool waits for a push that only this thread could make, and the wait never ends.

Rewriting `download.cc` to use it produces the server's startup line and nothing else, indefinitely.

Nexus has the same shape from the other side: `ASYNC_TEST` drives its graph itself and never sweeps the pump —
`tests/execute.cc` includes `thread_pump.hh` only to warn about a pump still registered after a run.
So a `co_await` on anything parked on an unthreaded semantic thread would hang there too.

## What to do about it

**A blocking get should work in a threads-off scenario**, and that it does not is the bug rather than a property to
document around.

Three items, roughly in order:

1. **`cc::async_blocking_get` on a pump-driven graph.**
   Either sweep the pump before parking, or make `participate_until_ready` yield to the registry rather than owning
   the thread.
   The examples' `await` goes away when this lands, and so do the equivalents hand-rolled in `sg::context`,
   `dx12_context`, the vulkan backend and blob-cache's test fixture.

2. **Nexus's async driver does not sweep the pump.**
   Same fix, one layer up: an `ASYNC_TEST` awaiting a pump-driven graph should progress rather than hang.

3. **`ASYNC_EXAMPLE`.**
   `EXAMPLE` is `NX_IMPL_TEST(name, ..., example, main_thread, ...)` and `execute.cc` asserts that `main_thread` and
   an async test cannot be combined — so the macro is `ASYNC_TEST` plus `example` and *without* `main_thread`, which
   moves an example's body off the process main thread.
   Free for a console example and not for a windowed one, so the config it carries is the decision rather than the
   macro.

   **The `main_thread` conflict is transient**, not a reason against the macro: it disappears once an async can be
   guaranteed to run on the main thread, which is wanted anyway.

None of this blocks clean-net.
It is recorded here because "why is there no `ASYNC_EXAMPLE`" and "why not just use `async_blocking_get`" are both
questions that will be asked again, and because the first two are real bugs that happen to have a workaround.
