---
created: 2026-11-27
refs:
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_upload_async.cc
  - test: sg - async upload interleaved with inline writes does not deadlock
---

# A vulkan async texture upload does not wait on a readback of the texture, and waiting hangs the GPU

A vulkan buffer upload waits on the buffer's pending readbacks (`job.download_wait`); a texture upload does not.
So an async download of a texture followed by an async upload into it is unordered on vulkan, a write-after-read the readback can lose.
dx12 orders both since 04984bd18, and so does vulkan for buffers.

04984bd18 gave vulkan's two texture upload paths the same wait, and on 2026-11-27 that hung the GPU under the full suite on an Arc B580.
`shaped-graphics-test --repeat 5` lost the device on iteration 2 three times, first noticed in `sg - async upload interleaved with inline writes does not deadlock`; vulkan logged `VK_ERROR_DEVICE_LOST` and dx12 `DXGI_ERROR_DEVICE_HUNG` straight after.
Without the two waits, ten iterations passed, and the test passes 60 repeats alone either way, so it is load that closes the cycle.
The waits are out again; where the cycle runs is not known.

A texture adds the layout fixup to what a buffer has: both directions submit one on the direct queue at enqueue, and a list's submit waits on a texture's download values whatever it does.
That is the likeliest edge the buffer case lacks, unverified.
This may also be what bug-sg-vulkan-device-lost-under-repeat has been catching all along, at a lower rate.
