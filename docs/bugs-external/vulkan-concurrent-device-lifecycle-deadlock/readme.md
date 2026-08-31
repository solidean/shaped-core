# NVIDIA: vkCreateRayTracingPipelinesKHR deadlocks against concurrent device create/destroy

**Status:** open, not yet filed upstream; worked around in shaped-core.
**Affects:** NVIDIA proprietary driver 591.86 on Windows, RTX 4090. **Not** AMD 2.0.5 on the same machine, same binary.
**Found by:** `shaped-graphics-vulkan-test` hanging past 120 s on Windows while passing in 4.6 s at `--jobs 1`.

## What happens

One thread inside `vkCreateRayTracingPipelinesKHR` blocks every other thread's `vkCreateDevice`, `vkDestroyDevice`, `vkCreateInstance` and `vkDestroyInstance` indefinitely.
It is not slowness: the repro's threads make no progress in 45 seconds, and in the original suite the process was still stuck after 120.

The stack that named it, from the hung test binary:

```raw
37  <idle pool worker>
 7  RtlEnterCriticalSection  <- vkDestroyDevice
 7  RtlEnterCriticalSection  <- vkGetInstanceProcAddr
 5  RtlEnterCriticalSection  <- vkCreateDevice
 3  RtlEnterCriticalSection  <- vkDestroyInstance
 2  RtlEnterCriticalSection  <- vkEnumerateDeviceExtensionProperties
 1  RtlAcquireSRWLockExclusive  nvoglv64.dll+0x4bf567  <- vkCreateRayTracingPipelinesKHR
```

Twenty-four threads on the loader's critical section, one inside the driver on a lock of its own.
The shape is a lock-order inversion between the loader's global lock and an NVIDIA-internal one.

## Why it is the driver and not us, and not the loader

`run.py` runs a matrix that removes one variable at a time.
All of it is `<vulkan/vulkan.h>` plus the C++ standard library — no shaped-core, no third-party code.

```
case                          hangs  attempts
----------------------------  -----  --------
churn only                    0/2    ok, ok
churn + compute pipelines     0/2    ok, ok
churn + validation            0/2    ok, ok
churn + raytracing pipelines  2/2    HUNG, HUNG
churn + raytracing + valid.   2/2    HUNG, HUNG
```

So it is not concurrent device lifecycle on its own, not pipeline creation on its own, and not the validation layer.
It needs a **ray-tracing** pipeline build specifically.

And the same binary, on the same loader, against the other adapter in this machine:

```
device 0 of 2: NVIDIA GeForce RTX 4090 (driver 591.86.0, ray tracing yes)   -> HUNG 2/2
device 1 of 2: AMD Radeon(TM) Graphics  (driver 2.0.5,  ray tracing yes)    -> ok   2/2
```

Both adapters advertise `VK_KHR_ray_tracing_pipeline` and run identical code.
One deadlocks and one does not, which puts it in the vendor driver rather than in the loader or in the calling code.

## Reproducing

```bash
uv run run.py                                   # the full matrix on device 0
uv run run.py --quick --device 1                # just the hanging case, on another adapter
uv run run.py --quick --threads 16 --repeat 5   # if it does not reproduce for you
```

Exit code 1 means it reproduced.
The script finds the Vulkan SDK through `VULKAN_SDK` and builds with whatever clang is on PATH; there is no shaped-core dependency and no `dev.py`.

The repro prints `HUNG` and calls `std::quick_exit` rather than joining, deliberately: the stuck threads are inside the loader and joining would hang the report too.
Attach a debugger before the harness kills it if you want the stacks yourself.

## The workaround in shaped-core

The exclusion lives in the library rather than in the test suite, because the constraint is the driver's and applies to any application on this hardware.

`vulkan_driver_lock.hh` holds a process-global `std::shared_mutex` with two guards, and the matrix above is what makes it a reader/writer lock rather than a mutex:
concurrent device create/destroy alone is fine, concurrent ray-tracing builds alone are fine, and only the two together hang.

- `scoped_raytracing_build` takes it **shared**, around `vkCreateRayTracingPipelinesKHR`.
  Builds still run in parallel with each other, which is the case that costs wall-clock.
- `scoped_device_lifecycle` takes it **exclusive**, across instance and device creation in `create_vulkan_context` and across the whole of `vulkan_context::shutdown`.
  A process creates a handful of devices, so the exclusive side is nearly free.

Shutdown takes it for the whole teardown rather than around `vkDestroyDevice` alone: the drain above it submits, and a build starting in that window would reopen the hole.

With it, the tier-2 suite runs its usual context per test at full concurrency without hanging.
It is slower than it would be sharing one device, because the exclusive guard serializes fifty device creations against the ray-tracing builds.
That is the price of per-test isolation rather than of the workaround.

**It is a workaround, not a fix.**
It removes the deadlock for anything going through sg, and an application reaching the driver another way is on its own.

## What to check when a new NVIDIA driver lands

Run `run.py`.
If `churn + raytracing pipelines` reports `ok` across several attempts, the driver is fixed: delete vulkan_driver_lock.hh/.cc and its three call sites, and this directory with them.
