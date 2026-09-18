# NVIDIA: vkCreateDevice deadlocks against a pending wait-before-signal on another device

**Status:** open, not yet filed upstream; not worked around in shaped-core yet.
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

## What to check when a new NVIDIA driver lands

Run `run.py`.
If the host-signal case reports `ok` across several attempts, the driver is fixed.
