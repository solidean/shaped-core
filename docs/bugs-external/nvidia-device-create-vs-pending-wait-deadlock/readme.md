# NVIDIA: vkCreateDevice deadlocks against a pending wait-before-signal on another device

**Status:** open, not yet filed upstream; worked around in shaped-core.
**Affects:** NVIDIA proprietary Vulkan driver on Windows 11, RTX 5070 Ti (driver version word `0x93d58000`).
**Found by:** `shaped-graphics-test --thorough` wedging about half its relwithdebinfo runs once the transfer fuzz ran in every backend's sweep.

## What happens

Device A has a queue submission waiting on a timeline value that has not been signalled yet.
That is legal Vulkan, and it is what an engine does when one queue must wait on a value another thread signals later.
While it is pending, another thread calls `vkCreateDevice` for device B.

`vkCreateDevice` never returns, and nothing can signal the value any more.
A queue submit that would signal it blocks, and so does a host `vkSignalSemaphore`, which involves no queue at all.
The process then cannot be killed: the creating thread is inside a kernel wait.

The stack that named it, from a nexus watchdog dump of the hung test binary:

```raw
vkCreateDevice (device B)                 vkQueueSubmit (device A)
  NtGdiDdDDIWaitForIdle                     RtlAcquireSRWLockExclusive
  nvoglv64.dll (as DrvPresentBuffers)       nvoglv64.dll+0x4bf567
  VkLayer_khronos_validation.dll            VkLayer_khronos_validation.dll
```

`vkCreateDevice` holds a driver lock and waits for the GPU to go idle.
The GPU cannot go idle while A's wait is pending, and every way of satisfying that wait needs the lock.
`nvoglv64.dll+0x4bf567` is the same lock the ray-tracing deadlock in [vulkan-concurrent-device-lifecycle-deadlock](../vulkan-concurrent-device-lifecycle-deadlock/readme.md) blocks on.

## Why it is the driver and not us

`repro.cc` is `<vulkan/vulkan.h>` plus the C++ standard library — no shaped-core, no third-party code.
Device A waits on one queue and signals from a second, as an engine's direct and copy queues do.

```
case                                  result
------------------------------------  ----------------------------------------------------------
create B, no pending wait on A        ok
pending wait on A, no create          ok
pending wait + create B, host signal  HUNG: B not created, vkSignalSemaphore never returned (unkillable)
```

Neither half hangs alone; together they always do.
The queue-signal case is the shape shaped-core hit, and the stack above is that one.

## Reproducing

```bash
uv run run.py            # the controls, then the hanging cases
uv run run.py --quick    # just the host-signal case
```

**Every hang leaves a process the OS cannot terminate until the next reboot.**
The controls run first for that reason, and `run.py` never waits on a child past its deadline, since waiting on an unkillable process would hang the script too.
The script finds the Vulkan SDK through `VULKAN_SDK` and builds with whatever clang is on PATH; there is no shaped-core dependency and no `dev.py`.

## Where shaped-core meets it

sg reserves an async upload's completion value when the upload is enqueued, and a later command list touching the destination waits on it on the GPU.
The copy actor signals it when it submits the copy, which is later.
So there is routinely a window in which a queue waits on a value not yet submitted, and any device created or destroyed in the process during it deadlocks.

The transfer fuzz makes that window frequent, with async uploads interleaved with command lists.
It used to run alone under `exclusive()`, so no device lifecycle overlapped it.
Since it runs in every backend's sweep, the `--thorough`-only vulkan never-block driver creates its device alongside it.

## The workaround in shaped-core

It follows the rule NVIDIA gives for wait-before-signal elsewhere (below): nothing may delay a signaller.
[forward_waits.hh](../../../libs/graphics/shaped-graphics/src/shaped-graphics/context/impl/forward_waits.hh) holds it, for every backend.

- Every completion timeline records the highest value any submission waits on and the highest value whose signal has been submitted.
  A timeline where the first is ahead has a pending wait-before-signal, and a process-wide count keeps how many do.
- `sg::impl::device_driver_barrier` wraps the driver's own `vkCreateDevice` and `vkDestroyDevice`.
  It waits for the count to reach zero, and while it is up a submission that would wait on an unsignalled value holds itself back until that signal is submitted.
- Signals come only from the transfer actors, whose waits name only each other's signals and never form a cycle, so both sides always finish.

It deliberately leaves the device lifecycle lock alone: an actor that had to take it could never signal a barrier's way out.
The cost is that creating or destroying a device now waits until the async transfers already in flight have submitted their signals.

## Known elsewhere

Searched in September 2026: no public report of this trigger — device creation against another device's pending wait, with even a host signal blocked.
The nearest material:

- NVIDIA's own [guidance on timeline wait-before-signal][driveos], for its Vulkan SC driver, states the rule this breaks.
  "For each counter value of a timeline semaphore that your application waits upon, the signaler must not be delayed by the waiters."
  Here the waiter delaying every signaler is the driver's own `vkCreateDevice`.
- `vkSignalSemaphore` blocking inside `nvoglv64.dll` under multithreaded use, on Windows with driver 466.77, with no NVIDIA reply ([forum][forum-signal]).
  Possibly the same lock; nothing there confirms it.
- `vkDestroyDevice` lock-order deadlocks inside the Linux driver, 535.216.01 ([forum, 2025][forum-destroy]).
  The same family of device-lifecycle locks, with a different trigger.

## What to check when a new NVIDIA driver lands

Run `run.py`.
If the host-signal case reports `ok` across several attempts, the driver is fixed.

[driveos]: https://developer.nvidia.com/docs/drive/drive-os/6.0.9/public/drive-os-linux-sdk/common/topics/graphics_content/Semaphore-Wait-Before-Signal-10.html
[forum-signal]: https://forums.developer.nvidia.com/t/vksignalsemaphore-call-is-blocked-frequently/205461
[forum-destroy]: https://forums.developer.nvidia.com/t/vkdestroydevice-hang-on-linux-libnvidia-glcore-so-535-216-01/319139
