---
created: 2026-11-09
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_query.cc
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-msl-benchmark.cc
closed: 2026-11-10
resolution: fixed
---

# metal runs out of counter heaps when many lists take timestamps, and every timestamp reads 0

sr's `bench_support.hh` `measure`, run with timestamps on metal, read 0 ms for every dispatch on an M4 (2026-11-09).
The log said, over 575 times: `[sg.metal] the metal device refused a timestamp counter heap: Cannot create heap; there are too many heaps already allocated`.
Beside it the driver logged `AGX: MTLCounterHeap: Invalid heap size requested`.
That run was 64 rounds of one timestamped dispatch per list, each list leasing a counter heap of its own.

Measured: the refusal and the zero readings.
Hypothesis, not checked: leased heaps go back to the pool too late, or never, so a burst of timestamped lists exhausts the device's few heaps; the heap-size message may be a second, separate fault.
A refused heap should also be reported as an error a caller sees, not a zero it would take for a time.
`sgl-against-msl-benchmark.cc` times by the wall clock for now, as the WebGPU one does.

## Resolved

Measured on an M4 on 2026-11-10: metal grants a process 32 counter heaps, at 64, 1024 or 4096 slots alike, and validation refuses one of 16384.
"Invalid heap size requested" is logged beside every refusal, so it is the same fault and not a second one.
The zeros were the timestamps of the refused heaps, which are invalid; the 32 that got a heap read correctly.
A list now leases a 64-slot range of a shared heap instead of a whole one, so one heap serves 64 lists in flight.
A refused heap is reported as a device error, not a warning.
`sg - timestamps from hundreds of lists in flight at once all read back` pins it on every backend: before the fix, 224 of its 256 lists on metal had invalid timestamps.
`sgl-against-msl-benchmark.cc` times by GPU timestamps again: one dispatch each, SGL at 0.96x to 1.00x MSL by median.
