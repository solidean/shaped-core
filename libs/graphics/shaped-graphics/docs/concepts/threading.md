# Concept: thread model

## What the thread model is

A backend declares, through [`sg::thread_model`](../../src/shaped-graphics/types.hh) reported by [`ctx.threading()`](context.md), how a context may be used across threads.
It is a contract on the caller about *this context*, never about whether the application has threads at all — that is `SC_THREADS`, and the two are independent.

```cpp
enum class thread_model
{
    main_thread,     // the bound calls on the main thread only
    single_threaded, // the bound calls on the creating thread only; any thread may create a context
    multi_threaded,  // the bound calls from any thread, with the synchronization listed below
};
```

## Free-threaded, whatever the model

**Anything returning an async, layouts and samplers may be called from any thread, on every backend.**

- **Asyncs move themselves.** A backend whose device lives on one thread starts its async work there before touching the device — `request_webgpu_context`, the async pipeline builds, the cached tier.
  A caller never needs to know where that is.
- **Layouts and samplers describe.** A binding-group layout or a pipeline layout validates and records its description at creation.
  Its backend objects are made on first use, which is always a bound call.
- **Dropping anything is free too.** A handle released off its device thread travels back there and is released at the next `advance_epoch`.

Resource creation is bound today, and nothing forces that: the same laziness would make it free-threaded (see [TODO](../TODO.md)).
Command lists, submission, presentation and epochs are bound by their nature — they are the device's queue.

`ctx.is_on_device_thread()` says whether the caller may make a bound call, and `ctx.device_home()` is where sg's own asyncs move to.

## What each value promises for the bound calls

**`main_thread`** — every bound call is made on the main thread.
A backend picks this when its device exists only in the main thread's realm: WebGPU in a threaded wasm build.
`device_home()` is the main thread's scheduler, and a bound call from elsewhere asserts.

**`single_threaded`** — every bound call is made on the thread that created the context.
Any thread may create one, and several contexts on several threads are fine; what is refused is moving one between threads.
It is the shape an OpenGL context has.
`device_home()` is null, since the creating thread has no home unless the application gives it one.

**`multi_threaded`** — split into two tiers:

- **Concurrency-safe**, callable from several threads at once:
  - resource and command-list operations — `create_command_list`, `create_raw_buffer`, `submit_command_list`, `drop_command_list`, and a resource's refcount reaching zero;
  - retire — `process_completed_epochs`, internally synchronized because the backends' own ring back-pressure invokes it from within concurrent recording;
  - the completion queries — `epoch_completion`, `submission_completion`, `is_submission_complete` — which read a fence and a guarded list;
  - awaiting `idle_completion` or `epochs_in_flight_completion`, which retire as they go and so share retire's one exclusion: neither may overlap advancing.
- **Externally synchronized:** advancing (`advance_epoch`, `try_advance_epoch`) and **`shutdown`**.
  The caller must guarantee none of these overlaps any other context operation.
  Advancing closes an epoch and rewrites the shared in-flight state, including the current-epoch counter every other op reads, so fencing it off is the caller's job.
  That is also why advancing is a deliberate, rationed operation (see [epochs](epochs.md)).

A command list is still **single-threaded per instance** regardless of the model: one thread records it, then submits or drops it once, in the epoch it was opened in.
But **several command lists may record concurrently**, even against the same resource.
Each takes an access-tracking slot that keys its private per-resource state, so their recording shares no mutable state.
See [barriers](barriers.md) for the slot model and the entry barrier each submit prepends.

## Builds without threads

Where `CC_HAS_THREADS == 0` — WebAssembly, or any build configured `-DSC_THREADS=OFF` — nothing about the API changes.
The transfer systems still hand their copies to a [`cc::threaded_actor`](../../../../base/clean-core/src/clean-core/thread/threaded_actor.hh).
The actor simply runs on whoever sweeps it instead of on a thread of its own.

**sg owns no pumping of its own.**
An unthreaded actor registers itself with clean-core's [pump registry](../../../../base/clean-core/src/clean-core/thread/thread_pump.hh).
Every blocking wait — `cc::async_blocking_get`, a frame loop, one of the waits below — sweeps that registry rather than draining the actors it happens to know about.
`cc::thread_pump_all()` is the whole entry point, and it costs one atomic load where every actor has a thread of its own.

The completion asyncs register a pump of sg's own, in the `sg::impl::completion_waiter` dx12 and vulkan own, standing in for the waiter thread it cannot start.
webgpu's stream system registers the other, driving its queued writes where nothing else would.
It settles what is due, then sweeps its siblings, and parks on the GPU only when no sibling made progress.
It parks only on work the GPU already has: the open epoch closes on an advance, and the thread that would advance is the one sweeping.
A GPU target may wait on a copy only an unthreaded actor signals, which is why the siblings run first.

sg used to carry `sg::context::pump()` and a per-backend `on_pump()` for this, and the reason they are gone is that they could only ever drain what *this context* could name.
A wait below sg, or beside it, saw none of them: the deadlock that produced the registry was `cc::async_blocking_get` sleeping on a store it had no way to reach.

The waits that need the sweep are

- `wait_for(future)` and `wait_for_ticks` / `wait_for_seconds` — the readback actor delivers the bytes.
- `wait_for_epoch`, and so also `wait_for_next_inflight_epoch` and `advance_epoch`'s throttle — *not* just a GPU wait.
  A submitted list can be parked on a resource's async-upload timeline, which the copy actor signals, so without the sweep the GPU never reaches the epoch fence.
- the inline-download ring's back-pressure and its drain-to-idle — only the actor frees ring space and decrements the outstanding count.

The last two are the traps: they look like waits on the GPU or on an atomic, not on an actor.
What the registry leaves as an obligation sits on the ACTORS rather than on the waits.
**A handler must not block on progress another registration has to make**, because unthreaded it holds the only thread there is.
The inline-download actor is the live example.
It sweeps until its submission completes and only then falls through to the fence wait, because that submission can be queued behind an upload the copy actor has not run yet.

## Whose work a transfer actor is doing

A transfer actor runs one caller's job on a thread that caller never sees, so a validation message or a check raised there would name nobody.
So every async transfer job carries a `cc::async_ambient_handle` captured on the enqueuing thread.
The actor installs it only for work that is that job's alone, such as polling its source or recording its copy.
Two rules keep that honest.

- **Never across other work.**
  A window that packs several jobs runs with no job's context, and so does any `cc::thread_pump_all()` the actor sweeps while it waits.
  A pumped component's report would otherwise fail a test that never started it.
- **Reset before settling.**
  The handle is dropped immediately before pushing anything the enqueuer awaits.
  That push resumes them, and a test ending right there would count the job's reference as async work it left running.

The handle owns a reference rather than holding a bare pointer on purpose.
A transfer whose future was dropped outlives the test that started it, and installing an unreferenced head then would walk a freed chain.

## Backends today

- **dx12** — `multi_threaded`.
  `create` / `submit` / `drop` are thread-safe: the open-command-list counter is atomic, and the command-allocator pool is mutex-guarded.
  The completion token is assigned together with the queue submit and fence signal under one lock, so token order equals signal order.
  `advance_epoch` and `shutdown` are externally synchronized.
- **vulkan** — `multi_threaded`, mirroring dx12.
  The open-command-list counter is atomic and the command-pool set is mutex-guarded.
  The completion token is assigned together with the `vkQueueSubmit` and timeline-semaphore signal under one lock, so token order equals signal order.
  `advance_epoch` and `shutdown` are externally synchronized.
- **webgpu** — `main_thread`.
  The bound calls assert at the device and queue accessors and at the command list's encoder, so a wrong thread is named before WebGPU fails obscurely.
  Its layouts make their WebGPU objects on first use, and a WebGPU handle dropped off main is released there at the next advance.

## See also

- [context](context.md) — the operations this classifies, and which scope each one lives on.
- [types.hh](../../src/shaped-graphics/types.hh) — the `thread_model` enum.
- [epochs](epochs.md) — why epoch management is the externally-synchronized half.
