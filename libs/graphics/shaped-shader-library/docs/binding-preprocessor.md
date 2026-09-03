# The binding preprocessor

The plan for a small HLSL-aware rewriting pass that owns binding addresses, and the typed C++ symbols it makes possible.

This supersedes the macro prelude explored on `jn/portable-hlsl`.
That approach is dead and the reason is recorded below, because it is the same reason a future session would reach for macros again.

## What this is

A shader declares its binding groups as **annotated namespaces** and writes no register, no space and no Vulkan annotation:

```hlsl
namespace frame_bindings //!> group 0
{
    Texture2D<float4> albedo;
    SamplerState linear_sampler; //!> static address=clamp_edge
}
```

That is valid HLSL as written.
It is also useless as written — nothing binds it correctly — and that is an accepted trade: the point of staying valid is that editors, formatters and language servers keep working on our shaders.

A preprocessing pass rewrites the inside of every annotated namespace before the real compiler sees it.
It adds `register(t0, space0)` on the DXIL arm and `[[vk::binding(0, 0)]]` on the SPIR-V arm.
The same parse, run at build time, generates a C++ struct with one named member per binding.
A caller then assigns resources to fields instead of spelling strings, and the group layout is a constant rather than something reflected.

The same annotation grammar carries four more things a shader currently states in C++ instead: a static sampler's state, an inline-constants block, a ray payload, and a vertex input struct.
For the last three the generator also emits the C++ struct that mirrors the shader's, so `sizeof` and `offsetof` replace hand-maintained numbers.

## Why not macros

The macro prelude got far enough to be judged, and it failed on one property.

**A prefix macro cannot emit `register()`.**
HLSL puts the address after the declared name and Vulkan puts it before, and a macro can only place tokens where it is called.
A macro that wraps the whole declaration can do it — `SC_TEXTURE(albedo, Texture2D<float4>)`.
But then the type is an argument, the declaration stops reading as HLSL, and every tool that parses the file sees a function call.

**Without `register()`, DXIL addresses are not stable.**
DXC assigns a register only to what an entry point actually references, so two stages of one pipeline disagree about a resource neither of them numbered.
Q8 in `portable-hlsl-spike-test.cc` pins it: one file, a vertex and a pixel stage sharing a texture preceded by a pixel-only one, gives `vs=t0 ps=t1`.
Q9 covers the other half — with an explicit `register()` the two agree, and the DXIL index matches the SPIR-V one.

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

**Q8 asserts its finding; Q9 and Q10 only log theirs, and Q10 walks past a rejected compile.**
Phase 1 fixes that, because a spike kept as the record of why this design is shaped the way it is has to fail when DXC changes under it.
Q9 wants two checks — that the two stages agree, and that the SPIR-V index matches.
Q10 wants the reflected name checked against the bare `Albedo`, and a `REQUIRE` where its `continue` is.

## The annotation grammar

One grammar carries every annotation:

```
//!> <name> [key=value]...
```

Three rules, and they are the whole grammar:

- **An attribute attaches to the declaration on its own line**, or — when it is alone on a line — to the next declaration.
  Long attribute lists get their own line, short ones ride along, and the parser needs one lookahead either way.
- **The first word is the attribute name**: `group`, `static`, `push_constants`, `payload`, `vertex_input`.
  A name the pass does not know is an error naming the line, never a comment it walks past.
- **The rest is `key=value`**, values being a bare token or a parenthesised tuple.
  No quotes, no nesting, no expressions.

**Values are `sg` enumerator names spelled exactly** — `clamp_edge`, `mirror_repeat`, `linear`, `less`.
A second vocabulary between HLSL and sg would be one more table to keep in step for no benefit, and every one of these names is already the one a C++ caller writes.

**A file is scanned whole.**
The pass looks for attributes everywhere, not only inside annotated namespaces, because `push_constants`, `payload` and `vertex_input` all attach at file scope.
Everything carrying no `//!>` attribute is passed through byte for byte, so a shader may keep hand-written `register()` declarations at file scope indefinitely.

### `group`

```hlsl
namespace <name> //!> group <n>
```

The marker carries the group index explicitly rather than assigning one by order of appearance.
Order-of-appearance was the `__COUNTER__` mistake in a new costume: a group shared by shaders in different files would number differently depending on which file the flattening started from.

The number is both the SPIR-V set and the HLSL space, so group `n` occupies `space<n>` and nothing else does.

**One annotated namespace is declared exactly once, in one file, in one block.**
Reopening it, nesting one inside another, or declaring the same annotated name in two files is an error the pass reports.
This is the invariant that lets the build-time generator and the runtime rewriter agree without ever talking to each other.
Both number a namespace's bindings by declaration order within its single block.

**One counter per group, across register classes.**
A group of a texture then a sampler becomes `register(t0, space0)` and `register(s1, space0)`, matching `[[vk::binding(0, 0)]]` and `[[vk::binding(1, 0)]]`.
That leaves gaps in each DXIL class's number line, which costs nothing: a register number is a name, not a position.
What it buys is that the same declaration has the same address on both targets.
Slot, declaration order and `sg::binding::index` are then one number, which is what lets a test compare addresses instead of only names.

### `static`

```hlsl
SamplerState linear_sampler; //!> static address=clamp_edge
SamplerComparisonState shadow; //!> static filter=(linear, linear, nearest) compare=less
```

A sampler binding marked `static` is baked into the pipeline layout's root signature rather than given a descriptor.
The keys are `sg::sampler`'s fields and everything omitted takes its default, which is a trilinear repeating sampler.
Two shorthands make it writable: `filter=linear` sets all three filters, `address=clamp_edge` sets all three axes.
The tuple form addresses them individually, in the field order `sg::sampler` declares.

The generated struct exposes what the shader declared as a constant, and `acquire_layout` also takes runtime samplers for a sampler the shader left undeclared.
**A declared sampler wins**: passing a runtime sampler for one the shader already declared is an error, not an override.

### `push_constants`

```hlsl
//!> push_constants space=9
ConstantBuffer<frame_constants> frame;
```

Inline constants — dx12 root constants, Vulkan push constants — reach a shader through `pipeline_layout_description::inline_constants` rather than through a group.
The register is always `b0`, since a pipeline layout carries at most one such binding, so the only number to state is the space.
That space matters: an inline-constants block sharing a space with a group's `b` registers is exactly the collision this pass exists to prevent.

Q8 applies here too, which is why the attribute must write a `register()` at all — a constants block referenced by one stage of a two-stage pipeline gets a register only in that stage.

`block_size` keeps coming from reflection, which is how a routine reads it today and is never wrong.

### `payload`

```hlsl
//!> payload
struct pt_payload { float3 radiance; float3 throughput; uint rng; float bsdf_pdf; };
```

`raytracing_pipeline_description::max_payload_size` is a byte count written by hand in C++ today, against a struct in another language.
Nothing in `sg::compiled_shader` reports it, so the pass computes it from the struct.

**A payload does not pack like a constant buffer.**
It is registers rather than a buffer, so the 16-byte row rule should not apply and its members should pack at natural alignment.
"Should" is doing work in that sentence, so the packing rule is pinned in the spike before the payload layout function is written.
Declare a payload whose two layouts differ, and read back what the driver actually reserved.

### `vertex_input`

```hlsl
//!> vertex_input
struct vs_input
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

//!> vertex_input slot=1 per_instance
struct cube_instance_input
{
    float3 center : TEXCOORD0;
    float3 half_extent : TEXCOORD1;
};
```

HLSL matches a vertex input by its semantic, and every other target matches by a numeric location.
So a portable shader carries both, and DXC spells the second `[[vk::location(N)]]` — an attribute with no DXIL meaning, which is why it forks the same way a binding's does.

**Members are numbered by declaration order.**
That is safe here in a way it was not for a group.
A group's numbering has to survive being shared across files, which is why its marker keeps an explicit number.
A vertex input struct is declared once, in one block, and its order is the order the reader sees.

`slot` and `per_instance` map onto `sg::vertex_input_slot`, which is per bound buffer.
So one annotated struct per slot is what `vertex_input_layout::create<Vs...>()` already expects.


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

**A struct carrying `push_constants`, `payload` or `vertex_input` is the one exception**, since its body is exactly what the generator has to reproduce.
There the pass reads scalars, vectors, matrices, arrays and nested structs — the full constant-block vocabulary — and anything else is an error naming the member.

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

**The table is checked against DXC rather than trusted.**
The generated `acquire_layout` compares its constant table against the compiled shader's reflected bindings — name, type, index, count, texture dimension, and a constant block's `block_size`.
The comparison is one-directional, because reflection reports only what the entry point referenced and its set is therefore a subset.

One comparison, two reactions.
At first acquire the table and the shader come from the same build, so a mismatch means the generator is wrong and `CC_ASSERT` is right.
After a hot reload the shader is legitimately newer than the table, so the same mismatch produces a failed shader.
That is an error on the async node naming the binding that moved and what changed about it, the way a broken edit already does.
So the comparison returns its difference rather than asserting internally, and each caller reacts.

## Where the pieces go

### The pass

`slib`, under `src/shaped-shader-library/binding/` — a tokenizer plus a rewriter, producing a list of edits `(offset, length, replacement)` applied to the original text.
Edits rather than a rebuilt string, so everything carrying no `//!>` attribute is provably untouched.

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

### The compile path

**The rewrite runs inside `shader_library::_compile_text`, between `preprocess` and `compile`.**
Not in a decorating compiler, and not inside a compiler at all.

`compile` is the right half and `preprocess` is not.
`_compile_text` calls `preprocess` to flatten includes, then hands the flattened text to `compile`.
Rewriting between them therefore sees one fully flattened, target-resolved translation unit — exactly the scope a group's numbering is defined over.
Rewriting before the flatten would see the entry-point file before its includes and miss every binding a header declares.

Putting it in `_compile_text` rather than in a compiler is what makes it unskippable.
`add_compiler` **replaces** any compiler registered for the same `(language, format)` edge.
So a decorator can be displaced by any later `add_compiler(create_dxc_compiler())` — in a test, an app, or a second library instance.
The result is a shader that compiles, runs, and reads from the wrong descriptor, and since the annotation is a comment nothing downstream can notice.
A `#error` guard in an included prelude cannot help either, because `preprocess` would evaluate it before the rewrite exists.

It also keeps the cache key honest.
`ssc::dxc` keys on a hash over the source, entry point, stage, model and options.
Rewriting before the compiler hands it text that is already rewritten, so everything the rewrite depends on is folded into the source and therefore into the key.
Rewriting inside the compiler would put a rewrite input outside the key, and two shaders differing only in that input would collide on one entry.

`_compile_text` already takes the target `format`, which is all the rewrite needs.

### Hot reloading

A reload re-runs `_compile_text`, so the rewrite runs on every reload exactly as on the first compile.

The case that needs an answer is a **registered** shader whose bindings changed on disk: its generated struct and constant table are from the last build and no longer describe it.
That fails the reload rather than asserting — an error on the async node, the way a broken edit already clears every pipeline built against the old layout and leaves the app running.
The log names the binding and what changed about it, not just "layout mismatch".

A shader compiled through `compile_source` has no generated struct and no table, so nothing constrains it and that path stays fully dynamic.

### The build-time generator

`GenerateShaderPackage.cmake` is replaced by a Python script the custom command invokes.
There is no C++ host tool: it complicates building and CI, and cross-compiling would mean building it for the host while the library builds for the target.

So the tokenizer exists twice — once in Python for the generator, once in C++ for the rewriter — and two tests are what make that safe.

- **A generated per-package self-check.**
  A package's `.cc` already embeds the full source of every shader.
  So the generator emits, beside the constant table, a check the C++ side runs.
  Parse the embedded source with `parse_binding_groups`, compare against the table Python produced from the same bytes, and fail with the first difference.
  Its corpus is every shader anyone declares, and it grows without anyone remembering to extend it.
- **A shared corpus file.**
  One data file of HLSL snippets and their expected parse, covering the supported subset and every construct the pass must reject **with the exact error it must report**.
  Both sides read it, so a grammar case is added once rather than twice.
  This is where `Texture2DMS<float4, 4>`, a non-literal array length, a nested namespace and a `typedef` get pinned, none of which will ever appear in a package.

Neither test proves the two parsers are the same function; they prove the two agree on what we ship and on what we thought of.
The reflection cross-check above is the third leg, and the only one that compares against DXC rather than against another parser.

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

### The generated group

For `frame_bindings` in namespace `shaders`:

```cpp
namespace shaders::frame_bindings
{
/// The bindings frame_bindings declares, in slot order. Generated; do not edit.
struct group
{
    sg::bound_view albedo;

    /// The samplers the shader declared `static`, ready to hand to acquire_layout.
    struct declared_sampler { cc::string_view name; sg::sampler sampler; };
    static constexpr declared_sampler static_samplers[] = { /* ... */ };

    /// The layout these declarations define — constant, so no reflection is consulted.
    /// The samplers overload supplies ones the shader did not declare; a sampler it did declare is an error.
    [[nodiscard]] static sg::binding_group_layout_handle acquire_layout(sg::context& ctx);
    [[nodiscard]] static sg::binding_group_layout_handle acquire_layout(sg::context& ctx,
                                                                       cc::span<sg::named_sampler const> samplers);

    /// Builds a group from the fields above, reporting any left unset.
    [[nodiscard]] cc::result<sg::binding_group_handle> create(sg::context& ctx) const;

    /// Binds at the group index the annotation gave, so no call site writes the number.
    static void bind(auto& scope, sg::binding_group const& g);
};
}
```

backed by a `constexpr` binding table the generator emits, which is what makes `acquire_layout` free of reflection.

`bind` is why binding a group by name needs no sg change: the namespace names the group, the marker numbers it, and the generated struct carries both.

### The generated mirror structs

A `push_constants`, `payload` or `vertex_input` struct also reaches C++, as a struct with explicit padding rather than as a byte count.

The number was never the interesting part — the layout is, and it is what nobody can check by reading.
`sv::frame_constants_gpu` is the current state of the art: a hand-written mirror with `_padding0[3]` and `_reserved[44]` placed by hand, and a `static_assert` on the total.
Its doc comment reads "Keep this in lockstep with common.hlsli".
A generated mirror makes that lockstep mechanical, and `sizeof` becomes true rather than asserted.

Getting it right means emitting HLSL's packing, not C++'s.
A constant buffer packs in 16-byte rows and an element may not straddle one, so `struct { float2 a; float3 b; }` is 32 bytes in HLSL and 20 in the naive C++ transcription.
Four edges are a silent wrong number if guessed rather than defined.
Matrices, where row-major is fixed; `bool`, four bytes in HLSL and one in C++; array stride, padded to 16 in a constant buffer and not elsewhere; and nested structs, row-aligned in a constant buffer.

Two `static_assert`s guard the result, and both are generated: the struct's total size against the size the generator computed, and **every member's `offsetof` against the offset it computed**.
Size alone would pass a mirror whose fields are in the wrong places and whose padding happens to add up.

The generated code spells its members as plain `u32` and friends, duplicating any small helper it needs rather than reaching for `sr::gpu_boolean`.
shaped-rendering sits above slib, so generated package code cannot see it.

### The generated vertex layout

A `vertex_input` struct produces one thing more than its mirror: the `sg::vertex_layout_of` specialization that says which bytes feed which input.

Everything that specialization states is already in the shader once the pass reads the struct.
The semantic comes from the member, the format from its type, the offset from the packing the mirror computes, the stride from the total, and `per_instance` from the attribute.
There are six hand-written ones in the tree today, and `examples/vdoc/cube-editor/cube_renderer.cc`'s four `TEXCOORD` attributes with their semantic indices are the most error-prone of them.
A mismatch between the shader's location order and the C++ attribute order builds a pipeline and draws wrong geometry, which is Q8's class of failure with no assertion anywhere.

**The hand-written path stays**, for a vertex type we do not own.
`sr::imgui_routine` specializes over `ImDrawVert`, a struct Dear ImGui owns and whose layout we cannot choose — there the generated struct is useless and the generated attribute identities are not.

This does not wait on sg's move to location-based vertex attributes, and makes it cheaper.
The generated layout carries the semantic today, exactly as a hand-written one does, and gains the `location` when `sg::vertex_attribute` grows one.
That is a change to a generator rather than to six specializations.


### The indexed bind path

`sg::binding_group_layout::bindings()` already documents that a binding's position **is** its slot index.
And `sg::binding_slot` already exists in shaped-graphics' `fwd.hh` as an opaque, typed index into that table — it is what `staging_binding_group::slot_of` returns.
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

**The reason is the generated struct, not the string compare it saves.**
The struct knows each binding's slot from the same parse that produced the shader's address, so there is no reason left for `create()` to look a name up and rediscover it.
The name comparison it skips is real but small — the groups built per frame in the tree today carry one to four bindings each.

What makes the slot correct is that a generated layout is built from the **full declared table**, in declaration order.
Every other layout in the tree is built from reflected bindings, where position `i` is a position in whatever subset that stage referenced.

`binding_slot` widens with it, from "meaningful only inside a `staging_binding_group`" to "a position in the layout's `bindings()`", and that doc change lands in the same commit.
A slot from the wrong layout would otherwise be in range, wrong and silent, where a wrong *name* is an error message today.
So the generated `create()` asserts the layout's `structural_hash` matches the one it was generated against.

**The one piece of genuine work here** is dx12's split layout.
It keeps separate `view_slots` and `sampler_slots` vectors, so a position in `bindings()` is not a position in `view_slots` once samplers interleave.
The slot must still be defined as a position in `bindings()`, because that is the backend-agnostic definition sg's own header commits to.
So `dx12_binding_group_layout::create` gains a `bindings()`-index to `view_slots`-index remap — the same mapping `descriptor_offsets_of` already computes for a staging group.
Vulkan indexes `bindings()` directly and needs nothing.

The backends should not grow a second copy of the 120-line group-building body.
Extract the per-slot work into a helper taking `(slot, bound_view const&)`, and make the name-based overload resolve-then-delegate.
That is the shape `staging_binding_group` already uses for its name-taking setters.
Error messages recover the name from `layout->bindings()[slot].name`, so nothing is lost.

## Phasing

1. **Clean up.**
   Delete the macro prelude, its CMake bake, its mount in `shader_library`, and revert the six shaders that were ported onto it.
   Keep the compile flags, the cross-stage bijection check, and the spike — all independent of the macro approach and all still correct.
   Turn Q9's and Q10's findings into assertions while the file is being kept.
   Rewrite `portable-hlsl.md` around this design, and re-point `shaders.md`.
2. **The tokenizer and `parse_binding_groups`**, with the shared corpus file behind them, covering the supported subset and every rejection with its error text.
   No rewriting yet — a parse that reports groups and bindings is independently checkable.
3. **`rewrite_binding_groups` in `_compile_text`**, with a test that one source compiles to both targets and reflects the same addresses.
   This is where Q8's failure becomes a passing test.
4. **The `path:binding:namespace` entry and the generated group**, the Python generator replacing the CMake one, and the per-package self-check.
5. **The mirror structs**, and the `push_constants`, `payload` and `vertex_input` attributes, after the spike pins the payload's packing.
   The generated vertex layout lands here too, since it is the same parse and the same packing engine.
6. **`sg::slotted_view`**, the `binding_slot` doc widening, the dx12 remap, and the resolve-then-delegate refactor in both backends.
7. **Port a real shader.**
   `imgui.hlsl` is the best first target: it has a texture, a static sampler and an inline-constants block, and its group is rebuilt whenever the bound texture changes.
   So it exercises the layout, the sampler declaration and the indexed path at once.

Steps 2 and 3 are worth landing before the rest is designed in detail.
The parse is what everything else is built on, and its subset will move once real shaders meet it.

## What this does not address

- **sv's bindless tables.**
  They need a register space per table, and nothing here assigns a space other than one per group.
  A binding declared outside an annotated namespace keeps its hand-written `register(t0, space5)`, so sv is not blocked by this design — it is simply not served by it yet.
