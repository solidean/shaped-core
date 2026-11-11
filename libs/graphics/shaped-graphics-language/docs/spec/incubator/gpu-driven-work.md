# GPU-driven work: work graphs, work lists, and creative schedulers

*Incubator: not normative.*

## The idea

**SGL should let a shader decide on the GPU what runs next, on every target: natively where the target has a way, polyfilled where it does not.**
Two native models exist, and they are not converging.

* **Work graphs** (D3D12, SM 6.8 and 6.9): nodes take typed records and emit records to other nodes, and the GPU schedules them.
  A node launches broadcasting (a grid per record), coalescing (a group gathers several records) or thread (one thread per record).
  Recursion is a node feeding itself to a declared depth, and SM 6.9's mesh nodes end a graph in a draw.
  Vulkan's only counterpart is `VK_AMDX_shader_enqueue`, provisional and AMD's alone.
  **Microsoft has frozen them**: reported as supported in SM 6.8 and 6.9 and not in SM 6.10, for lack of adoption.
* **Work lists** (D3D12, announced as the direction after work graphs): `ExecuteIndirect` grown so that each record picks its pipeline from a GPU-resident program table.
  That is a draw, a dispatch or a ray dispatch per record, chosen by a shader.
  Vulkan already has the shape in `VK_EXT_device_generated_commands`, whose indirect execution sets switch a pipeline per sequence.
  Metal has it in indirect command buffers, which can set a compute or render pipeline per command.
  So work lists are the model with a native answer on three targets, and work graphs the model with one on one.

**One SGL model should cover both**, so a program says what produces what and the target picks the mechanism.
A node is an entry point whose input is a record type and whose outputs are typed record streams with a declared bound, which the checker verifies the way it checks a mesh payload.
On dx12 with work graphs, that is a work graph.
Everywhere else, and on dx12 if measurement says so, it is a polyfill.

## Polyfills: native where supported, polyfilled where not

**Whether a polyfill is automatic or opt-in is decided by measurement, per polyfill and per target.**
A polyfill that runs within a small factor of native, or of the hand-written alternative, is automatic, and `require` of its feature is satisfied by it.
One that does not stays opt-in: the program, or the host at the pipeline, asks for it by name.
Each polyfill therefore ships with a benchmark beside the native path on a device that has both, which is what decides, and what reopens the decision when drivers change.

The candidates for GPU-driven work, which are not exclusive:

* **Level by level.**
  A record queue per node in a buffer; each round, a small compute pass turns the queues' counts into indirect dispatch arguments, and every node with records runs.
  vkd3d-proton emulates D3D12 work graphs on Vulkan with ordinary compute this way, which is prior art and a correctness reference.
  Needs only compute and `dispatch_groups_indirect`, so it reaches webgpu.
  The cost is a round trip per graph depth and queues sized for the worst case.
* **A persistent megakernel.**
  One dispatch whose workgroups pull records from atomic queues and `switch` on the node, the move raytracing-futures makes for a pipeline on webgpu.
  No round trips, and it keeps divergent nodes from waiting on each other.
  But no API promises forward progress between workgroups, and a spin on a queue can hang on Apple and mobile GPUs, so it is the hazardous one.
* **Ray generation with SER as the scheduler.**
  A raygen shader may reorder its threads by a coherence hint with no trace at all (`MaybeReorderThread(hint, bits)` in DXR 1.2, and its Vulkan counterpart).
  The hardware then regroups threads that took different branches, which is what a divergent compute kernel and a thread-launch node both want.
  So a node graph, or a plain compute kernel with heavy divergence, could run as a raygen that loops: pop a record, reorder by its node, run the node.
  The restrictions are real: no workgroup memory and no barriers in a raygen, no subgroup layout to rely on after a reorder, and only dx12 and vulkan with SER.
  Whether it beats compute is a measurement, not a guess.
* **Work lists as the polyfill for work graphs.**
  Where a target has work lists but no work graphs, the level-by-level loop's dispatches become one work list per round, each record naming its node's pipeline.

Work lists themselves need a polyfill where there are none: on webgpu, and on dx12 until they ship.
That is the level-by-level loop with one node per pipeline, or a binning pass and one indirect dispatch per pipeline.

## What it touches

* The language: node entry points, record types, typed output streams with bounds, and recursion as a node that names itself with a declared depth.
* The checker: record bounds, the graph's acyclicity up to declared recursion, and what a node may do under each launch mode.
* Emitting: DXIL and HLSL node shaders on dx12; a compute kernel per node, a scheduler kernel and a megakernel for the polyfills; a raygen form for the SER scheduler.
* Host generation: the graph's description and the backing memory's size, generated as a ray-tracing pipeline's description is today (A7 in the comparison matrix).
* sg: a work-graph state object and `DispatchGraph` on dx12, device-generated commands on vulkan, indirect command buffers on metal.

## Already fixed by the syntax

* A stage is an entry point with an attribute, whose interface is values, so a node is one more attribute rather than a new mechanism.
* `require` is how a program opts into a feature, and a polyfilled feature is one `require` stops refusing.
* A mesh payload is already a typed, bounded record from one stage to the next, which is the shape a node's output stream generalizes.

## Open

* Whether work graphs are worth native support at all given the freeze, or only as the reference a polyfill is measured against.
* What D3D12 work lists actually specify: nothing here was read from a primary source yet, and the design waits on that.
* Whether the SGL model is the graph (nodes and records) or the list (records naming a pipeline), with the other derived from it.
* How a polyfill sizes its queues: a declared bound per node, a host-given budget, or overflow reported under the `report` profile.
* Whether the SER scheduler is a polyfill chosen by measurement or an explicit tool a program asks for, for divergent compute in general.
