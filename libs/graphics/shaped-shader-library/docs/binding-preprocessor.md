# The binding preprocessor

The plan for a small HLSL-aware rewriting pass that owns binding addresses, and the typed C++ binding structs it makes possible.

This supersedes the macro prelude explored on `jn/portable-hlsl`.
That approach is dead and the reason is recorded below, because it is the same reason a future session would reach for macros again.

## What this is

A shader declares its binding groups as **annotated namespaces** and writes no register, no space and no Vulkan annotation:

```hlsl
namespace frame_bindings //!> group 0
{
    Texture2D<float4> albedo;
    SamplerState linear_sampler;
}
```

That is valid HLSL as written.
It is also useless as written — nothing binds it correctly — and that is an accepted trade: the point of staying valid is that editors, formatters and language servers keep working on our shaders.

A preprocessing pass rewrites the inside of every annotated namespace before the real compiler sees it.
It adds `register(t0, space0)` on the DXIL arm and `[[vk::binding(0, 0)]]` on the SPIR-V arm.
The same parse, run at build time, generates a C++ struct with one named member per binding.
A caller then assigns resources to fields instead of spelling strings, and the group layout is a constant rather than something reflected.

## Why not macros

The macro prelude got far enough to be judged, and it failed on one property.

**A prefix macro cannot emit `register()`.**
HLSL puts the address after the declared name and Vulkan puts it before, and a macro can only place tokens where it is called.
A macro that wraps the whole declaration can do it — `SC_TEXTURE(albedo, Texture2D<float4>)`.
But then the type is an argument, the declaration stops reading as HLSL, and every tool that parses the file sees a function call.

**Without `register()`, DXIL addresses are not stable.**
DXC assigns a register only to what an entry point actually references, so two stages of one pipeline disagree about a resource neither of them numbered.
Q8 in `portable-hlsl-spike-test.cc` pins it: one file, a vertex and a pixel stage sharing a texture preceded by a pixel-only one, gives `vs=t0 ps=t1`.
Q9 pins the other half — with an explicit `register()` the two agree, and the DXIL index matches the SPIR-V one.

So the address has to be written into the source, and something other than the preprocessor has to write it.
That is the whole argument for this pass.

## What the spikes already settled

`libs/graphics/shaped-shader-compiler-dxc/tests/portable-hlsl-spike-test.cc` is DXC behaviour pinned as tests.
Three of its findings are load-bearing here and are worth re-reading before changing anything:

- **Q8** — without an explicit `register()`, a stage that references a subset of the declarations gets different DXIL registers than one that references more.
  This is why the pass exists.
- **Q9** — with an explicit `register()`, DXIL and SPIR-V agree on the index, and both stages agree with each other.
  This is why the pass works, and it means a cross-target check may compare addresses rather than only names.
- **Q10** — a resource declaration may live in an HLSL namespace, on both targets, and **reflection reports the bare name** (`albedo`, not `frame_bindings::albedo`).
  This is why a namespace can carry the group without sg's name matching changing at all.

Q10 also carries the warning that shapes one rule below: two namespaces may each declare the same symbol, so a namespace does **not** give the duplicate-slot protection a file-scope symbol would.
The pass has to detect collisions itself.

## The annotation

```hlsl
namespace <name> //!> group <n>
```

The marker is `//!>` immediately after the namespace's opening line.
It carries the group index explicitly rather than assigning one by order of appearance.
Order-of-appearance was the `__COUNTER__` mistake in a new costume: a group shared by shaders in different files would number differently depending on which file the flattening started from.

**One annotated namespace is declared exactly once, in one file, in one block.**
Reopening it, nesting one inside another, or declaring the same annotated name in two files is an error the pass reports.
This is the invariant that lets the build-time generator and the runtime rewriter agree without ever talking to each other.
Both number a namespace's bindings by declaration order within its single block.

Everything outside an annotated namespace is passed through byte for byte.
A shader may keep hand-written `register()` declarations at file scope indefinitely; the pass never looks at them.

## The supported subset

Deliberately small to start.
Inside an annotated namespace the pass understands:

- A declaration `Type name;` and `Type name[N];`.
  `N` is a decimal literal and nothing else — no `constexpr`, no arithmetic, no identifiers.
- A `Type` that is a bare identifier or an identifier with a simple `<...>` argument list, where the arguments are themselves identifiers, keywords or literals.
  `Texture2D<float4>`, `StructuredBuffer<my_vertex>` and `Texture2DMS<float4, 4>` are in; anything needing real template parsing is out.
- Line and block comments, and string literals, skipped correctly so a `//` inside a string does not end a line.

Not supported, and reported as an error rather than passed through silently:

- `typedef`, `#define`, a nested namespace, a `struct`/`cbuffer` body, or a function definition inside an annotated namespace.
- An array length that is not a literal.
- A declaration whose type is not in the table below.

An error names the file, the line and what it could not parse.
A shader that trips the subset moves the construct outside the namespace; the restriction is on where bindings are declared, not on what a shader may contain.

## The type table

One table maps an HLSL type name to a register class and an `sg::binding_type`.
It is the single most important piece of shared state in this design, because the rewriter and the C++ generator must agree on it exactly.
A divergence is a resource bound to the wrong descriptor, with nothing to catch it.

| HLSL | class | `sg::binding_type` |
|---|---|---|
| `Texture1D/2D/3D/Cube` and `*Array`, `Texture2DMS` | `t` | `readonly_texture` |
| `RWTexture1D/2D/3D` and `*Array` | `u` | `readwrite_texture` |
| `Buffer`, `StructuredBuffer` | `t` | `readonly_structured_buffer` |
| `RWStructuredBuffer` | `u` | `readwrite_structured_buffer` |
| `ByteAddressBuffer` | `t` | `readonly_raw_buffer` |
| `RWByteAddressBuffer` | `u` | `readwrite_raw_buffer` |
| `ConstantBuffer` | `b` | `uniform_buffer` |
| `SamplerState` | `s` | `sampler` |
| `SamplerComparisonState` | `s` | `sampler` |
| `RaytracingAccelerationStructure` | `t` | `acceleration_structure` |

A texture type also yields the `texture_view_dimension` the generated layout needs.

## Where the pieces go

### The pass

`slib`, under `src/shaped-shader-library/binding/` — a tokenizer plus a rewriter, producing a list of edits `(offset, length, replacement)` applied to the original text.
Edits rather than a rebuilt string, so everything outside an annotated namespace is provably untouched.

The parse result is also the generator's input, so it is a value type worth naming:

```cpp
struct slib::shader_binding_group
{
    cc::string name;                    ///< the namespace's name
    u32 group = 0;                      ///< from the annotation
    cc::vector<sg::binding> bindings;   ///< in declaration order; index is the slot
};

[[nodiscard]] cc::result<cc::vector<shader_binding_group>> parse_binding_groups(cc::string_view hlsl);
[[nodiscard]] cc::result<cc::string> rewrite_binding_groups(cc::string_view hlsl, sg::shader_format target);
```

`parse_binding_groups` is what the generator wants; `rewrite_binding_groups` is what the compiler wants; both run the same tokenizer.

### The compiler frontend

`slib::shader_compiler` has four virtuals: `source_language`, `target_format`, `preprocess` and `compile`.
`shader_library::add_compiler` **replaces** any compiler already registered for the same `(language, format)` edge.
So the frontend is a decorator that owns another compiler and forwards everything except `compile`:

```cpp
class binding_rewriting_compiler final : public slib::shader_compiler
{
    std::unique_ptr<shader_compiler> _inner;
    // source_language / target_format / preprocess: straight forwards
    // compile: rewrite_binding_groups(desc.source, _inner->target_format()), then _inner->compile
};
```

`compile` is the right half and `preprocess` is not.
`shader_library::_compile_text` calls `preprocess` to flatten includes, then hands the flattened text to `compile`.
Rewriting in `compile` therefore sees one fully flattened, target-resolved translation unit — exactly the scope a group's numbering is defined over.
Rewriting in `preprocess` would see the entry-point file before its includes and miss every binding a header declares.

Registering it needs no change to slib: wrap `create_dxc_compiler()`, add the wrapper, and it displaces the plain one.

### The build-time generator

`sc_add_shader_package` takes entries `path:stage:entry_point`.
This adds a fourth kind, `path:binding:namespace`:

```cmake
sc_add_shader_package(
    TARGET     graphics-rotating-cube-example
    NAME       cube_shaders
    SHADERS
        cube.hlsl:vertex:main_vs
        cube.hlsl:fragment:main_ps
        cube.hlsl:binding:frame_bindings
)
```

**A binding entry generates only from the named file, never from its includes.**
An `.hlsli` that declares a group is registered on its own, in whichever package owns it — otherwise every shader including it would generate the same struct again.
This is the one asymmetry in the design worth remembering.
The runtime rewriter is per *flattened translation unit* and rewrites everything it sees; the generator is per *file* and emits only what that file declares.
They agree because a namespace's numbering is local to its single block.

The generated struct, for `frame_bindings` in namespace `shaders`:

```cpp
namespace shaders::frame_bindings
{
/// The bindings frame_bindings declares, in slot order. Generated; do not edit.
struct group
{
    sg::bound_view albedo;
    sg::sampler linear_sampler;

    /// The layout these declarations define — constant, so no reflection is consulted.
    [[nodiscard]] static sg::binding_group_layout_handle acquire_layout(sg::context& ctx);

    /// Builds a group from the fields above, reporting any left unset.
    [[nodiscard]] cc::result<sg::binding_group_handle> create(sg::context& ctx) const;
};
}
```

backed by a `constexpr` binding table the generator emits, which is what makes `acquire_layout` free of reflection.

### The indexed bind path

`sg::binding_group_layout::bindings()` already documents that a binding's position **is** its slot index.
And `sg::binding_slot` already exists in `fwd.hh` as an opaque, typed index into that table — it is what `staging_binding_group::slot_of` returns.
So the indexed path is a new key on an existing concept rather than a new concept:

```cpp
/// A layout slot and what is bound to it — the index-keyed twin of sg::named_view.
struct sg::slotted_view
{
    binding_slot slot = binding_slot::invalid;
    bound_view view;
};
```

Named `slotted_view` rather than `indexed_binding` on purpose.
`sg::binding` is a *declaration* and `named_view` is a *supply*.
Calling the new type `indexed_binding` would read as a `binding` carrying an index, which every `binding` already has.
And `binding::index` is the register number, a different integer from the slot.

What it skips is real.
`create_binding_group` currently does an O(views × slots) `cc::string` comparison per call.
Transient groups are rebuilt per frame, and per *draw* in imgui's texture-switch loop.

**The one piece of genuine work here** is dx12's split layout.
It keeps separate `view_slots` and `sampler_slots` vectors, so a position in `bindings()` is not a position in `view_slots` once samplers interleave.
The slot must still be defined as a position in `bindings()`, because that is the backend-agnostic definition sg's own header commits to.
So `dx12_binding_group_layout::create` gains a `bindings()`-index to `view_slots`-index remap.
Vulkan indexes `bindings()` directly and needs nothing.

The backends should not grow a second copy of the 120-line group-building body.
Extract the per-slot work into a helper taking `(slot, bound_view const&)`, and make the name-based overload resolve-then-delegate.
That is the shape `staging_binding_group` already uses for its name-taking setters.
Error messages recover the name from `layout->bindings()[slot].name`, so nothing is lost.

## The open decision: where the tokenizer runs at build time

The rewriter runs at runtime, in C++, inside slib.
The generator runs at build time, and today `GenerateShaderPackage.cmake` is **pure CMake script mode** — no Python, no host tool, invoked as `cmake -P`.

The two must agree on the parse exactly.
Three ways to get there:

- **One tokenizer in C++, wrapped in a small host tool** that the custom command invokes.
  One implementation, no drift possible.
  Costs a host-tool build dependency, which is a real cost when cross-compiling — the tool must be built for the host while the library builds for the target.
- **One tokenizer in Python**, invoked by the custom command, with the C++ rewriter calling nothing and the runtime pass reimplemented.
  Same drift risk as the third option, and adds Python to the build, which `GenerateShaderPackage.cmake` currently avoids on purpose.
- **Parse twice** — CMake regex for the generator, C++ for the rewriter.
  No new dependency, and a standing risk that the two disagree on some declaration and bind a resource to the wrong descriptor.

**Recommendation: the host tool.**
The failure mode of drift is a silently wrong binding, which is the exact class of bug this whole design exists to remove.
Paying a build dependency to make it impossible is the trade this repo usually makes.
Worth deciding before any code is written, because it decides where the tokenizer lives.

## Phasing

1. **Clean up.** Delete the macro prelude, its CMake bake, its mount in `shader_library`, and revert the six shaders that were ported onto it.
   Keep the compile flags, the cross-stage bijection check, and the spike — all independent of the macro approach and all still correct.
   Rewrite `portable-hlsl.md` around this design, and re-point `shaders.md`.
2. **The tokenizer and `parse_binding_groups`**, with tests over the supported subset and every rejection in it.
   No rewriting yet — a parse that reports groups and bindings is independently checkable.
3. **`rewrite_binding_groups` and the decorating compiler**, with a test that one source compiles to both targets and reflects the same addresses.
   This is where Q8's failure becomes a passing test.
4. **The `path:binding:namespace` entry and the generated struct**, plus the host tool if that is the decision.
5. **`sg::slotted_view`**, the dx12 remap, and the resolve-then-delegate refactor in both backends.
6. **Port a real shader.**
   `imgui.hlsl` is the best first target: it has a texture and a static sampler, and its group is rebuilt per draw.
   So it exercises the layout, the sampler match and the indexed path at once.

Steps 2 and 3 are worth landing before 4 and 5 are designed in detail.
The parse is what everything else is built on, and its subset will move once real shaders meet it.

## What this does not address

- **sv's bindless tables.** They need a register space per table, and nothing here assigns a space other than one per group.
  A binding declared outside an annotated namespace keeps its hand-written `register(t0, space5)`, so sv is not blocked by this design — it is simply not served by it yet.
- **Inline constants.** Still declared at file scope with whatever the shader writes today.
  Whether they should get an annotation of their own is a later question.
- **Vertex input locations.** Unrelated to bindings; still needs the `[[vk::location]]` fork or a pass of its own.
