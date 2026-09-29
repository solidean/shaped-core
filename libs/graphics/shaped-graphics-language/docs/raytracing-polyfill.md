# The ray-query polyfill: pool, layout and binding

For whoever works on the webgpu ray-query polyfill, on either side of it.
sg's webgpu backend builds acceleration structures into the layout below, and SGL's prelude traverses them.
**This is an internal contract, not part of the language.**
A shader that reads the pool other than through `trace` is undefined, and nothing here promises stability across versions.
Both sides change together, in one commit.

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
| 1 | header: x node count, y first instance unit, z instance count, w kinds present (bit 0 triangles, bit 1 boxes) |
| 2 per node | the nodes; a leaf's primitives are instance indices |
| 7 per instance | the instances |

An instance, 7 units:

| unit | holds |
|---|---|
| 0 1 2 | world to object, the three rows of the 3x4 matrix |
| 3 4 5 | object to world, the three rows of the 3x4 matrix, as `tlas_instance::transform` wrote it |
| 6 | x the BLAS region's unit, y `instance_id` (24 bits) and `mask` (bits 24 to 31), z `hit_group_offset` (24 bits) and flags (bits 24 to 31), w the instance's index in the TLAS |

The instance flags are:
* bit 24: forced opaque;
* bit 25: forced non-opaque;
* bits 26 and 27: the triangle cull mode (0 back, 1 front, 2 none).

## BLAS region

| units | holds |
|---|---|
| 1 | header: x node count, y first primitive unit, z primitive count, w kind (0 triangles, 1 boxes) |
| 2 per node | the nodes; a leaf's primitives are primitive indices within this BLAS |
| 3 or 2 per primitive | the primitives |

A triangle, 3 units, in object space and pre-fetched, so traversal never reads an index buffer:

| unit | x y z | w |
|---|---|---|
| 0 | vertex 0 | the primitive index within its geometry |
| 1 | vertex 1 | the geometry index |
| 2 | vertex 2 | 1 when the geometry is opaque, else 0 |

A box, 2 units:

| unit | x y z | w |
|---|---|---|
| 0 | minimum | the primitive index within its geometry |
| 1 | maximum | the geometry index, with bit 31 set when the geometry is opaque |

## Building

Every build is recorded compute work, ordered like any other command, so a trace recorded after it sees the result.

* **The tree is built over the primitive order, with no spatial sort.**
  Consecutive runs of up to 4 primitives are the leaves, and each level above pairs consecutive nodes.
  The topology is fixed by the counts alone, so the CPU writes it and the GPU fills in the boxes, one dispatch per level.
  A mesh in a coherent triangle order traces well, and a triangle soup traces slowly.
* **A TLAS box is an instance's BLAS root box, transformed to world space**, by one dispatch before the levels.

## Traversal

The traversal is SGL source in the prelude, `@internal`, so no user code resolves it.
The WGSL emitter lowers a `trace` to it, and the interpreter runs the same source in tests.

* Two levels with one stack: a TLAS leaf pushes its instances' BLAS roots, with the ray moved into object space.
* Triangles are tested watertight, so a ray through a shared edge hits exactly one of its triangles.
* A candidate the any-hit decision must see is a non-opaque triangle or any box, with the instance's forced opacity and the ray flags applied first.
* `t` and the barycentrics agree with hardware within a tolerance, never bit for bit.
