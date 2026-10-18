# WARP on Windows on ARM: a pointer-authentication fastfail out of the pixel JIT

**Status:** open, not yet filed upstream; the trigger is not yet isolated, so there is no `repro.cc` yet.
Worked around in shaped-graphics, which no longer takes the adapter it happens on for hardware.
**Affects:** WARP (`d3d10warp.dll`) on Windows 11 arm64, GitHub's `windows-11-arm` image, October 2026.
x64 WARP is not affected.
**Found by:** `shaped-viewer-test` exiting `0xC0000409` with no output at all on the Windows ARM MSVC job, once sv started rasterizing.

## What happens

A pixel shader runs in WARP's JIT, on a WARP worker thread.
The JIT code takes an access violation, and WARP handles it: the process carries on.
When `d3d10warp!PixelJITProcessor::ExecuteJIT` later returns, its `autibsp` rejects the return address.
That is a `__fastfail` with subcode `0x49`, `FAST_FAIL_POINTER_AUTH_INVALID_RETURN_ADDRESS`.

A fastfail goes straight to the kernel.
No exception filter runs, so clean-core's crash handler prints nothing, and buffered output is lost.
The run reads as a silent exit, a few seconds in.

## Why it is the driver and not us

Captured by running the test binary under the arm64 `cdb` on the runner, passing first-chance access violations back to the program:

```
(2218.1ab4): Access violation - code c0000005 (first chance)
00 0x00007ff4`d9ae1e60                                     <- JIT code
01 d3d10warp!PixelJITProcessor::ExecuteJIT+0x2c
   ... the same on two more WARP threads, each handled ...
(2218.e8c): Security check failure or stack buffer overrun - code c0000409 (!!! second chance !!!)
Subcode: 0x49 FAST_FAIL_POINTER_AUTH_INVALID_RETURN_ADDRESS Pointer authentication (PAC) return address violation
d3d10warp!PixelJITProcessor::ExecuteJIT+0x40:
00007ffa`747071f8 d50323ff autibsp
```

Every frame on the faulting thread is WARP's own; the only graphics driver the process loads is `d3d10warp.dll`.
The access violation is WARP's to take and to handle, and the return address it then fails to authenticate is its own.

## The adapter it happens on

The runner has no GPU, and DXGI enumerates the Basic Render Driver twice:

```
adapter 0: "Microsoft Basic Render Driver" vendor 0x1414 device 0x008c software-flag 0 d3d12 1
adapter 1: "Microsoft Basic Render Driver" vendor 0x1414 device 0x008c software-flag 1 d3d12 1
```

Adapter 0 carries no `DXGI_ADAPTER_FLAG_SOFTWARE`, so anything asking for "the first adapter that is not software" gets WARP.
That is how a job meant to run no dx12 test ran every sv "hardware" test on WARP, and hit this.
x64 hosts with no GPU flag the Basic Render Driver as software, which is why the other GPU-less jobs never saw it.

## What does not trigger it

A pixel shader and a compute shader reading a 64-element `StructuredBuffer` through a bounding descriptor, at offsets 0, 32, 64 and 2^20, built with FXC (`ps_5_0` / `cs_5_0`).
Every read is correct on that runner, on both adapters, the out-of-bounds ones returning 0.
So the handled access violation is not simply WARP's bounds check, or not the DXBC path to it.
What sv rasterizes when it happens is DXIL: shader model 6.8, 16-bit types, and bindless descriptor tables.

## Reproducing it

Not standalone yet.
In the tree: `uv run dev.py test shaped-viewer-test` on Windows on ARM, with `SC_DX12_ADAPTER` unset and the workaround below reverted.

## What shaped-core does about it

sg's dx12 backend counts every adapter of Microsoft's vendor id, `0x1414`, as software, flag or not.
`is_software_adapter` in [dx12_context.create.cc](../../../libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_context.create.cc) is the rule.
So `SC_DX12_ADAPTER=hardware` hides it as it hides WARP anywhere else, and the Windows ARM job runs no dx12 test, as [ci.md](../../guides/ci.md) intends.

sv's own Windows-on-ARM skips predate this and name the inline readback path instead.
They are very likely this fault as well, met before anyone had a stack.
[bug-sv-woa-inline-readback-fastfail](../../../libs/graphics/shaped-viewer/docs/issues/bug-sv-woa-inline-readback-fastfail.md) says so where it lists them.

## Re-checking after an update

The workaround is right regardless, since Microsoft ships no GPU.
What to re-check is WARP: on a Windows on ARM machine, run sv's tests with `SC_DX12_ADAPTER=warp`.
If they pass, WARP is fixed there, and sv's Windows-on-ARM skips can go.
