# Reserved bindings

A reserved binding is a resource no program declares and sg binds itself.
A program cannot name one, and the host never hears of one.
`sg_debug`, the window `print` and `assert` write into, is the first, and more are planned.

## The contract

sgl and sg each hold one side, and neither links the other.
This page is the agreement, and two tables state it:

| side | table | what it is for |
|---|---|---|
| sgl | `check::k_reserved_bindings` in [`check/reserved_bindings.hh`](../src/shaped-graphics-language/check/reserved_bindings.hh) | what a compile declares, and which stages reach it |
| sg | `sg::reserved_bindings` in [`binding/reserved_bindings.hh`](../../shaped-graphics/src/shaped-graphics/binding/reserved_bindings.hh) | what each backend binds, and what a reflection may report |

**Neither table is derived from the other, and the build holds them equal.**
slib is the one library that sees both.
[`reserved_contract.cc`](../../shaped-shader-library/src/shaped-shader-library/compiler/reserved_contract.cc) is a `static_assert` over every name and address, and over the debug window's header.
A table edited on one side alone stops the build.

| entry | dx12 | vulkan | WebGPU | metal | enabled by |
|---|---|---|---|---|---|
| `sg_debug` | `u0, space11`, a root UAV | set 3, binding 19 | group 3, binding 19 | `[[buffer(30)]]` | diagnostics |
| `sg_acceleration_pool` | `t0, space12`, a root SRV | set 3, binding 17 | group 3, binding 17 | none | a software trace |
| `sg_acceleration_roots` | `b0, space12`, root constants | set 3, binding 18 | group 3, binding 18 | none | a software trace |
| `sg_array_lengths` | `b1, space12`, root constants | set 3, binding 20 | none | none, the array's table counts it | an unbounded binding array |
| `sg_dispatch_grid` | `b2, space12`, root constants | set 3, binding 21 | group 3, binding 21 | none, metal dispatches the grid's threads | a bare `@compute` |

On metal a ray-tracing pipeline's raygen alone takes `sg_debug` at its slot, and hands it every stage it calls through `sgl_context` (EMIT-183).

`sg_array_lengths` is a block of 16 lengths, `uint4[4]`, one per unbounded binding array of the bound groups in the order of their groups.
sg writes it at each dispatch and draw from the groups bound, beside the roots, and an emit for dx12 or vulkan rewrites `xs.length` into a read of it (CHK-491).

`sg_dispatch_grid` is the threads `dispatch_threads` asked for, `uint4`, which a bare `@compute`'s threads past the grid return before (CHK-520).
A pipeline of such a shader is built over its layout's twin that holds it, `pipeline_layout_description::dispatch_grid`, so no other layout pays for it.

## The debug window

The window is 32-bit words, a header of three and then records.

| word | holds |
|---|---|
| 0 | the cursor: the next record's first word, counted from the end of the header |
| 1 | the capacity, in words |
| 2 | how many records did not fit |

A record is the site's id, its own length in words, then each value's lanes in order.
A lane is one word: a float's bits, an int's, a bool as 0 or 1, and a `half` widened to a float first.
A record reserves its words with one atomic add on the cursor, so it is written whole or counted as dropped.
sg decodes it through the site table, which reaches `sg::compiled_shader` as `debug_sites`.

## How a compile reaches one

**An entry with a declaration is an ordinary resource everywhere below the prelude.**

1. `registry` writes each entry's declaration into the prelude as an `@internal` binding, so its types exist and no program finds it (CHK-323).
2. Legalize writes statements over it (LEGAL-70), and lists the binding in `flat_entry_point::reserved`, not in `bindings`, so no group number moves.
3. `plan` appends it to `resources` at the table's address, with `is_reserved` set.
   Those are the only lines that treat it differently: it takes no group and reaches no host list, no layout and no footprint.
4. Each writer declares it as it declares any resource, and metal takes it as a function parameter at its slot.

So neither a text writer nor the IR route holds any logic of `print` or `assert`.
A stage the table says cannot reach an entry has its prints erased, and a failed assert still ends the invocation.

**The acceleration pool and roots are table entries by name and address alone, with no declaration.**
Their addresses, slib's WGSL reader and the reflection exemption all read the tables, but `write_acceleration_pool` still declares them, and a builtin's text still reads them.
What stops the rest of the move is the roots: `cbuffer` on HLSL, root constants on dx12 and a uniform on WebGPU, which `mut buffer[...]` in the prelude does not say.
Nothing below the text reads either yet, since a ray query does not lower natively.

## Adding one

1. Add an entry to `check::k_reserved_bindings`, with its declaration, its address on each target, its stages, and what enables it.
   Its one member's type says what kind of resource it is.
2. Add the same name and addresses to `sg::reserved_bindings`, at the same position.
   The build refuses until the two agree.
3. Regenerate the prelude: `uv run dev.py run sgl -- prelude --write libs/graphics/shaped-graphics-language/prelude/builtins.sgl`.
4. Bind it in each sg backend, at the table's address, in the pipeline layout.
5. Write statements over it in legalize, with `reserved_symbol_of`, `binding_member` and `buffer_element`.

No writer changes.
