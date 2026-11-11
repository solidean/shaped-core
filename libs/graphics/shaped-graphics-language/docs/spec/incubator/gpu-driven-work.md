# GPU-driven work: work lists, and creative schedulers

*Incubator: not normative.*

## The idea

**SGL should let a shader decide on the GPU what runs next, on every target: natively where the target has a way, polyfilled where it does not.**
The model is **work lists**: a buffer of records a shader writes, each naming the pipeline of one draw, compute dispatch or ray dispatch.

* **dx12**: D3D12 work lists, announced as an extension of `ExecuteIndirect` with a GPU-resident program table; not read from a primary source yet.
* **vulkan**: `VK_EXT_device_generated_commands`, whose indirect execution sets switch a pipeline per sequence.
* **metal**: indirect command buffers, whose commands may each set a render or compute pipeline.
* **webgpu**: none, and dx12 has none until work lists ship, so both take a polyfill.

A record type is an ordinary struct, and the pipelines a record may name form a program table the host code generator emits, as it emits a ray-tracing pipeline's description today.
The checker verifies that every pipeline a record names is in the table, and that a record matches what its pipeline takes.

**Work graphs are not part of it.**
D3D12 has deprecated them: reported as supported in SM 6.8 and 6.9 and not in SM 6.10, for lack of adoption.
Their only Vulkan counterpart is AMD's provisional `VK_AMDX_shader_enqueue`, so SGL adds them neither natively nor polyfilled.
What a work graph expresses, a node producing records for other nodes, is a work list whose records a later round consumes.

## Polyfills

**Whether a polyfill is automatic or opt-in is decided by measurement, per polyfill and per target.**
A polyfill that runs within a small factor of native, or of the hand-written alternative, is automatic, and `require` of its feature is satisfied by it.
One that does not stays opt-in: the program, or the host at the pipeline, asks for it by name.
Each polyfill therefore ships with a benchmark beside the native path on a device that has both, which is what decides, and what reopens the decision when drivers change.

The candidates, which are not exclusive:

* **Level by level.**
  A record queue per pipeline in a buffer; a small compute pass turns the queues' counts into indirect dispatch arguments, and every pipeline with records runs.
  Needs only compute and `dispatch_groups_indirect`, so it reaches webgpu.
  The cost is a round trip whenever records produce more records, and queues sized for the worst case.
  vkd3d-proton emulates D3D12 work graphs on Vulkan this way, which makes it prior art for the scheduling, if not for the model.
* **A persistent megakernel.**
  One dispatch whose workgroups pull records from atomic queues and `switch` on the pipeline, the move raytracing-futures makes for a pipeline on webgpu.
  No round trips, and it keeps divergent work from waiting on itself.
  But no API promises forward progress between workgroups, and a spin on a queue can hang on Apple and mobile GPUs, so it is the hazardous one.
* **Ray generation with SER as the scheduler.**
  A raygen shader may reorder its threads by a coherence hint with no trace at all (`MaybeReorderThread(hint, bits)` in DXR 1.2, and its Vulkan counterpart).
  The hardware then regroups threads that took different branches, which is what a divergent compute kernel wants, and a queue of mixed records too.
  So a divergent kernel, or a work list's records, could run as a raygen that loops: pop a record, reorder by its pipeline or branch, run it.
  The restrictions are real: no workgroup memory and no barriers in a raygen, no subgroup layout to rely on after a reorder, and only dx12 and vulkan with SER.
  Whether it beats compute is a measurement, not a guess.

## What it touches

* The language: record types, and a way for a shader to append a record naming a pipeline.
* The checker: that a record's pipeline is in the table and takes that record, and the bounds a polyfill's queues are sized by.
* Emitting: nothing new where a target is native; a scheduler kernel, a megakernel or a raygen form for the polyfills.
* Host generation: the program table and the record layout, generated as a ray-tracing pipeline's description is today (A7 in the comparison matrix).
* sg: device-generated commands on vulkan, indirect command buffers on metal, and D3D12 work lists once they ship.

## Already fixed by the syntax

* `require` is how a program opts into a feature, and a polyfilled feature is one `require` stops refusing.
* A mesh payload is already a typed, bounded record from one stage to the next.
* A `pipeline` declaration already names its stages, so a table of pipelines is a list of names the checker resolves.

## Open

* What D3D12 work lists actually specify: nothing here was read from a primary source yet, and the design waits on that.
* How a polyfill sizes its queues: a declared bound, a host-given budget, or overflow reported under the `report` profile.
* Whether the SER scheduler is a polyfill chosen by measurement or an explicit tool a program asks for, for divergent compute in general.
