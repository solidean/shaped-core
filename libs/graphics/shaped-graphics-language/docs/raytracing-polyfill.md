# The ray-query polyfill: pool, layout and binding

For whoever works on the ray-query polyfill, on either side of it.
sg builds acceleration structures into the layout below, and SGL's prelude traverses them.
webgpu always does, and dx12 and vulkan do for a package that takes `SOFTWARE_TRACE`.
**This is an internal contract, not part of the language.**
A shader that reads the pool other than through `trace` is undefined, and nothing here promises stability across versions.
Both sides change together, in one commit.
What a shader writes is the language's, [spec/raytracing.md](spec/raytracing.md), and EMIT-135 is what WGSL declares for it.

## Binding

A pipeline layout on webgpu that holds an `acceleration_structure` binding gets two more entries in the reserved group (`sg::reserved_binding_group`, group 3).
Binding 0 is the inline constants and bindings 1 to 16 are the bound samplers, so these follow them:

```wgsl
@group(3) @binding(17) var<storage, read> sg_acceleration_pool: array<vec4u>;
@group(3) @binding(18) var<uniform> sg_acceleration_roots: array<vec4u, 4>;
```

* **The pool** is one storage buffer per context, holding every BLAS and TLAS of that context.
  It grows by reallocating and copying, and a region's offset never changes while the structure lives.
* **The roots** are a uniform with a dynamic offset, written by the backend per dispatch or draw into its constant pages, as inline constants are.
  The root of the entry point's `k`-th acceleration member, counting across its binding list in order, is `sg_acceleration_roots[k / 4][k % 4]`.
  An entry point lists at most 16 acceleration members.
* **The acceleration member itself takes no WGSL binding.**
  sg's webgpu layout keeps its slot in the group's numbering, so every other member's slot is what it is on the other targets.

dx12 and vulkan carry the same two where a group's binding says `traced_in_software`, which is the one way a layout pays for them.

* **dx12:** a root SRV at `t0, space12`, and 16 root constants at `b0, space12`, set right before each op.
* **vulkan:** bindings 17 and 18 of set 3, a storage buffer and a uniform buffer.
  Such a set is written per op into a transient range of the descriptor heap, which also holds the roots it names.
  A run of ops under one pipeline and one set of roots shares a range.
* **The pool** is one host-visible buffer per context, written as a build records.
  Growing it copies it to a new one, and the old one stays until no recorded work can read it.

## Units

Every offset is an index into `sg_acceleration_pool`, a unit of 16 bytes.
Floats are stored as their bits and read with `bitcast<f32>`.
A region is contiguous and starts with a one-unit header.

**Unit 0 is the empty TLAS**: a header whose node count is 0.
An unbound or null TLAS resolves to root 0, so every trace against it misses, as sg's null acceleration structure does elsewhere.

## Nodes

Both levels use one node format, BVH2, two units per node:

| unit | x y z | w |
|---|---|---|
| 0 | box minimum | inner: left child's node index; leaf: first primitive index |
| 1 | box maximum | inner: right child's node index; leaf: primitive count, with bit 31 set |

Node indices count from the first node of the region, which is its root.
A leaf holds at most 4 primitives.
A box is closed: a ray touching its surface enters it.

## TLAS region

| units | holds |
|---|---|
| 1 | header: x node count, y first instance unit, z instance count, w kinds present (bit 0 triangles, bit 1 boxes) and bit 2 where an instance moves |
| 2 per node | the nodes; a leaf's primitives are instance indices |
| 7 per instance, 10 where one moves | the instances |

An instance, 7 units:

| unit | holds |
|---|---|
| 0 1 2 | world to object, the three rows of the 3x4 matrix |
| 3 4 5 | object to world, the three rows of the 3x4 matrix, as `tlas_instance::transform` wrote it |
| 6 | x the BLAS region's unit, y `instance_id` (24 bits) and `mask` (bits 24 to 31), z `hit_group_offset` (24 bits) and flags (bits 24 to 31), w the instance's index in the TLAS |

The instance flags are:
* bit 24: forced opaque;
* bit 25: forced non-opaque;
* bits 26 and 27: the triangle cull mode (0 back, 1 front, 2 none);
* bit 28: the instance moves.

In a TLAS whose header sets bit 2, every instance takes 10 units, and units 7 to 9 are its object-to-world rows at time 1.
A static instance writes its transform there again; a moving one's units 0 to 2 are unread, since the walk inverts the map at the ray's time.
Its TLAS nodes bound the instance over the whole interval, as the boxes of both its transforms do.

## BLAS region

| units | holds |
|---|---|
| 1 | header: x node count, y first primitive unit, z primitive count, w kind (0 triangles, 1 boxes) and bit 2 where its triangles move |
| 2 per node | the nodes; a leaf's primitives are primitive indices within this BLAS |
| 3 or 2 per primitive, 6 per triangle that moves | the primitives |

A triangle, 3 units, in object space and pre-fetched, so traversal never reads an index buffer:

| unit | x y z | w |
|---|---|---|
| 0 | vertex 0 | the primitive index within its geometry |
| 1 | vertex 1 | the geometry index |
| 2 | vertex 2 | 1 when the geometry is opaque, else 0 |

In a BLAS whose header sets bit 2, every triangle takes 6 units, and units 3 to 5 are its vertices at time 1, their `w` unused.
A still geometry of that BLAS writes its vertices there again.
Its nodes bound the triangle over the whole interval, as the box of both poses does, widened by 1e-6 of the coordinates, since the mix rounds.
A TLAS box over it needs nothing more: a point of a moving instance is a mix of two places of a point in that box.

A box, 2 units:

| unit | x y z | w |
|---|---|---|
| 0 | minimum | the primitive index within its geometry |
| 1 | maximum | the geometry index, with bit 31 set when the geometry is opaque |

## Building

On webgpu every build is recorded compute work, ordered like any other command, so a trace recorded after it sees the result.
On dx12 and vulkan a build computes the same words on the CPU, boxes included, from the description's `cpu_*` spans.

* **The records are sorted by the Morton code of their box centers first**, quantized to 10 bits an axis over the centers' own box.
  A GPU radix sort does it, after the records are written and before any box is, for a BLAS of triangles or boxes and for a TLAS alike.
  So a record's position says nothing about its primitive: the index a hit reports is the one the record carries in its `w` word.
  Fewer than 16 records, a build asking `fast_build`, or more than the 128 MiB scratch binding holds, keep the caller's order.
  Unsorted, a shuffled mesh traced up to 2500 times slower than the same mesh sorted (perf-sg-polyfill-unsorted-tree).
* **The tree is built over that order.**
  Consecutive runs of up to 4 records are the leaves, and each level above pairs consecutive nodes.
  The topology is fixed by the counts alone, so the CPU writes it and the GPU fills in the boxes, one dispatch per level.
* **A TLAS box is an instance's BLAS root box, transformed to world space**, by one dispatch before the levels.
  It is widened on each axis by 1e-6 of that axis's centre and half extent together, since the transform rounds and the box must still hold every point of the instance.

## Traversal

The traversal is SGL source in the prelude, `@internal`, so no user code resolves it.
The WGSL emitter lowers a `trace` to it, and the interpreter runs the same source in tests.
A test's driver binds a pool and each acceleration member's root (EVAL-95), and `tests/interpret/emulated-trace-test.cc` writes its pools by hand from this page.

* Two levels with one stack: a TLAS leaf pushes its instances' BLAS roots, with the ray moved into object space.
* A BLAS leaf is read as its region's header `w` says, triangles or boxes, so a mixed TLAS needs no marker of its own.
  The TLAS header's kinds are not read yet.
* **Triangles are tested watertight**, as Woop, Benthin and Wald do: the ray is sheared onto +z through the origin, and a triangle's three edges are judged in that plane.
  A rounded difference of rounded products is 0 or has the true sign, so only a tie is left to decide, and no target needs doubles for it.
  A tie goes one way for an edge taken in its sorted direction, as if the ray stood infinitesimally beside every edge at once.
  So two triangles sharing an edge take a ray through it exactly once, whatever their winding, and a fan takes a ray through its corner exactly once.
  Sorting an edge has both triangles evaluate one expression, so a target that fuses a product still negates it exactly.
  A product that rounds to a tie it is not can still decide a corner wrongly, within a rounding of that corner.
  Only vertices equal bit for bit are shared, so an edge between two instances need not be.
* **A box is entered a little past its sides**, by the rounding of the slab test's products, and a ray running along a side enters it, so a watertight triangle behind a box's face is reached.
* A stack of nodes is written before it is read, so neither of its arrays is filled.
* The intersection runs for every box the ray enters.
  The any-hit decision sees a non-opaque triangle, or the report of a non-opaque box, with the instance's forced opacity and the ray flags applied first.
* `t` and the barycentrics agree with hardware within a tolerance, never bit for bit.
