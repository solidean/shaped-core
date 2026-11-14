---
created: 2026-11-14
priority: next
tags: [dx12, testing]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_expected_messages.hh
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_context.create.cc:301-316
  - issue: perf-sg-optional-clear-value
---

# A shaped-viewer run raises thousands of allowlisted debug-layer warnings

**A run that logs warnings at frame rate is not a passing state, even when every one is allowlisted.**
The allowlist was meant for a rare advisory sg provokes on purpose; it now hides a steady stream, and anything new drowns in it.

## Measured

A 30 s run of shaped-viewer-test on dx12 (`--stop-after 30 --rec`, 2026-11-14) recorded 19,509 `debug layer: {}` warnings, about 650 a second.
They are 62 distinct texts:

| count | message |
|---|---|
| 14,254 | `ClearRenderTargetView`: no clear value passed to resource creation |
| 2,450 | `ClearDepthStencilView`: the same, for depth |
| ~1,600 | `BuildRaytracingAccelerationStructure`: resources sharing a GPU virtual address range on one heap |
| ~1,200 | `CreateConstantBufferView`: the same, at view creation |

About 14,500 of them were logged outside any profiling scope, the rest inside `sv.hybrid`, `sv.raster_bound`, `sv.pathtrace` and `sv.wavefront`.

## Why no test fails

`dx12_expected_messages.hh` allowlists both families by substring, and every dx12 test driver (sg, sr, sv) allows the list in **every** test of its binary.
So nexus' log rule never sees them as undeclared.
The rule's intent, that a passing test logs no warning it did not declare, holds by the letter only.

## What is wrong with that

- **Applications get the same stream.** The allowlist lives in the test drivers; an app with the debug layer on logs every one of them as a warning.
- **The noise is most of a recording.** It is a fifth of the log events in a test's `.ccrec`, and the bulk of their bytes.
  The compacted test recording drops what `NX_ALLOW_LOGS` allows, so this one is handled; the stream itself is not.

The address-range entry is benign in practice: only sg places resources, and the escape hatch for a caller's own placement is unused.
Narrowing it to the transient case would mean parsing the message text, so it stays as it is.

## To investigate

- **The clear value.** Does sv clear its targets to a fixed value often enough that `perf-sg-optional-clear-value` removes most of the clear messages?
  Or should sg pass the value it knows at creation where a caller declares one?
- **Where an accepted advisory is filtered.** A decided advisory could be denied by message ID in the info queue that sg installs, for apps and tests alike, instead of a substring per test driver.
  An ID is narrower than a substring and does not differ between drivers.

## Done when

- A normal shaped-viewer run on dx12 logs no debug-layer warning at all.
- Each remaining allowlist entry is an ID-level decision with its reason, and nothing in it fires at frame rate.
