---
created: 2026-11-12
refs:
  - test: "sg - a polyfilled task workgroup that launches nothing draws nothing, and the rest keep their order"
  - test: "sg - an SGL task workgroup that launches no mesh workgroup draws nothing, beside ones that do"
  - test: sg - an indirect draw of a polyfilled task stage launches what its task workgroups do
---

# The polyfilled task stage's tests read 0x7F where they expect 0x80, on every backend of an NVIDIA laptop

On an RTX 5070 laptop (Windows, relwithdebinfo), the three tests fail on dx12, dx12 native-dxil, vulkan and vulkan native-spirv.
Each mismatch is one unit in the red channel's lowest byte: `raw(0x7F000000_...) == raw(0x80000000_...)`, the drawn color 127 where the test expects 128.
Seen on origin/main at 53652a1a5 checked out alone, so it is not from local work.
A color of exactly 0.5 written to an 8-bit target lands on either side of the rounding, so the expectation is likely the thing to loosen.
