# Why: emitting

*Tracer: deliberately thin.*

The reasons behind the rules of [emitting.md](../emitting.md).
Nothing here is normative.

The emitters follow the [compilation model](../../incubator/compilation-model.md): SGL compiles to shader text, and the text's own compiler optimizes it.
So an emitter is a printer with opinions about names and addresses, and it transforms nothing.

## EMIT-2

One HLSL text could carry both APIs' addresses, `register` and `[[vk::binding]]` side by side, since each compiler reads only its own.
The split is kept for what still differs, and that is little: where the inline constants live, and that SPIR-V has locations where dx12 has semantics.
That little is exactly what a reader of a capture wants to see, so each target says it in the text.
Both share one dialect in the implementation, and a fourth target is one more file.

MSL came as that one file: a dialect, a reserved-word list and a case, and the walk over the tree did not change.

## EMIT-4

A matrix's orientation in HLSL can come from `-Zpr` or from a `#pragma pack_matrix` that the text cannot see.
Either one would transpose the cube with every size still correct, and nothing would report it.
So every matrix member says `column_major`, and the product is written `mul(m, v)`, which means the same under either flag.
A flag a text needs to compile at all is a different thing: without it the compile fails loudly, and nothing reaches the screen.
That is why HLSL's 16-bit float is `float16_t` and never `half`: without `-enable-16bit-types`, `half` silently means a 32-bit float, where `float16_t` fails.

## EMIT-6

The stages of a pipeline are compiled apart by every API, and hot reload replaces one of them.
A text that needed its sibling would make the pair the unit, and a vertex stage is reused with several pixel stages.
What the two share is the stage link, and its addresses come from member order alone, so both stages compute the same ones without talking.

## EMIT-13

A shader that compiles for dx12 and fails for the web is found late, by whoever builds the other platform.
An error that every target reports is found by whoever wrote the line.
So a rule one target needs is a rule of the language: a layout WGSL cannot express is refused for HLSL too.

## EMIT-21

The host asks for an entry point by name, so it used to be an error for a name any target reserved.
The text now reports the name it declares, and slib compiles by that name, so a rename is invisible to the host.
An error for a name that is only a problem in one target was then the bigger surprise, and EMIT-20 renames instead.

## EMIT-40

DXC packs a push-constant block tightly, whatever `-fvk-use-dx-layout` says, so `{float3; float; mat4}` would sit elsewhere than on dx12.
Nothing reports the difference: both modules compile, and the shader reads its members from the wrong bytes.
An offset on every member makes SPIR-V agree with the one layout the host fills, and it is there for the reader too.
A nested struct states its own members' offsets on its declaration, which is the one struct every use of it shares (EMIT-114).
dx12 gets no attribute, since DXC warns about a `vk::` attribute it ignores there.

## EMIT-46

A shader debugger and a capture tool show the target text and never the SGL source.
Somebody who opens a capture should find `let lit: vec3f = p.color * (...)` and recognize the line they wrote.
A minified text would be a little smaller and would make every such session start with a translation.

## EMIT-55

HLSL has no expression that builds a struct of the program, so a local is the only spelling.
Assigning member by member keeps the field names in the text, which a positional constructor would lose.
Every flat expression is pure, so building a struct ahead of the statement that uses it changes nothing.

## EMIT-57

`using namespace metal;` makes every name of the standard library visible in the global scope, where the program's structs are declared.
Metal also declares names there of its own, such as the `quad` it reserves for a type it never defines, and its headers define macros.
A struct called `filter`, `length` or `quad` would then be ambiguous at its first use, and the compiler would name a header nobody wrote.
So the list holds all of them, found by declaring each identifier of the toolchain's headers as a struct and as a function.
[tools/msl-probe](../../../../tools/msl-probe/readme.md) is that probe, and re-running it regenerates the list.
A local only hides such a name, so reserving it there costs an underscore and nothing else.
Declaring the program in a namespace of its own was the alternative, and it was declined: it would make MSL's entry points qualified names, and minting is how every other target handles a taken name.
`main` is no keyword, and MSL refuses a function of that name, so by EMIT-20 an entry point called `main` is `main_` in MSL.

## EMIT-58

MSL has no global resources: whatever a stage reads is a parameter of its entry point.
The body still reads `constants.view_projection`, since a reference parameter is used like the global the other targets declare.
sg's metal backend binds group N at buffer index N and keeps four such slots, so 4 is the first index no group can take.
The backend binds the inline constants there, as `k_inline_constants_buffer_index`.
A vertex buffer has no index in the text at all: `[[stage_in]]` reads through the pipeline's vertex descriptor.

## EMIT-62

MSL's `float3` is sixteen bytes, so the `float` that HLSL and WGSL put into its tail would start a new row in MSL.
Refusing the block, as this rule once did, refused the most common struct in GPU memory on every target because of one.
`packed_float3` is twelve bytes, MSL converts it to and from `float3` on assignment, and a component reads as it does on a `float3`.
SPIRV-Cross writes the same for the same reason.
The layout of a vertex input is not the struct's: `[[attribute(i)]]` reads through the vertex descriptor, so EMIT-62 is about GPU memory only.

## EMIT-112

One C++ struct copied into what every target reads needs one layout, and the targets' own rules disagree on it.
HLSL's is the one the rules take: dx12 then needs nothing, and every other target can be made to reach it.
Rejecting what one target cannot place natively was the alternative, and it rejects a `{float; float3}` block and a buffer of `float3` everywhere because of one target.
A rule of GPU alignment instead, as std430 and WGSL place, only moves the cost.
A `float3` member then needs padding in the host struct that nobody wrote, and dx12 needs a `packoffset` on every member.
MSL's 16-byte `float3` would still need a memory form.
WGSL can only raise an alignment, never lower one, so a vector it would align further than SGL's offset becomes scalars; MSL has packed vectors for the same case.
Vulkan's relaxed block layout refuses a few of these layouts: a member packed into a nested struct's last row, a vector across a row of a buffer's element, a 12-byte stride.
`scalarBlockLayout` admits all of them, and nearly every Vulkan device has it, so sg's vulkan backend requires it rather than SGL narrowing its rules for devices without it.
Only the root that needs it becomes a memory form: the rest reads in a capture as the source does (EMIT-46), and the form is invisible to the program.

## EMIT-116

Reordering members is how a compiler packs a struct tighter, and it is planned: a `{float; float4; float}` block takes three rows where `{float4; float; float}` takes two.
Promising declaration order now would make that a breaking change for every host that computed an offset by hand.
The generated struct already follows whatever the compiler chose, so a host that uses it loses nothing.
A host that needs a fixed layout, such as memory shared with a hand-written struct, will ask for one by annotation rather than by accident.

## EMIT-63

MSL is column-major with the vector on the right, as WGSL is, so there is no `mul` and nothing to state about orientation.
It could build a struct with braces, `pixel_input{a, b, c}`, which drops the member names just as a positional constructor would.
`const` stands in front of the type because the hand-written MSL in this repo writes it there.

## EMIT-64

An unsuffixed literal is a `double` in C++, and MSL has no `double`, so the question is fair.
The hand-written MSL in this repo and the MSL SPIRV-Cross generates both write `0.5` for a `float`.
The Metal compiler takes the unsuffixed form, so it stays.
A suffix would be the only place where one literal is spelled differently per target.

## EMIT-74

An emitter that switches over the builtins is a second list of them, and there were five such lists: one per target, the arity for the validator, and the layouts.
Each had to agree with the prelude and with the interpreter, and nothing but a failing test said when one did not.
A record holds all of it, so adding a builtin touches one place, and a target that spells a function differently is one field of that record.
The few calls that are no call and no operator - a matrix product, a position that gains its `w` - write themselves through a function of the record.
That function is given the arguments already written and nothing else of the emitter, so it cannot reach into a target's state.

## EMIT-75

WGSL refuses a bare call of a function whose value must be used, and every function of its library is such a function.
The phony assignment `_ = value;` is what the language offers for it.
HLSL takes the bare statement, and MSL, which is C++, would warn about an unused value without the cast.

## EMIT-77

EMIT-5 writes exactly the structs and the binding an entry point needs, so writing every case of an enum rather than the ones some arm names looks like a contradiction.
It is not: the unit that is needed is the enum, and a reader comparing the text against the source wants its set rather than the subset this entry point happened to match on.
An enum is small and closed, so the cost is bounded by the declaration; a struct's members are written whole for the same reason.
A `default` arm also stands for the cases nobody named, and a reader who cannot see them cannot tell what it covers.

## EMIT-86

A register is an address the host has to agree with, and the host's side of it is sg's.
Its dx12 backend reads a group's slot i as register i of the group's space, and its vulkan backend as binding i of the group's set.
SGL already owns both numbers, the group from the entry point's list and the slot from the binding's members, and writes them in WGSL.
So HLSL writes the same pair, and the text DXC compiles is the text `sgl emit` prints.
slib's binding pass exists so that hand-written HLSL serves dx12 and vulkan at once, and it stays HLSL-only.
SGL knows every address and writes each one explicitly, so it has no use for the pass.
Growing the pass to WGSL and MSL would cost more than SGL does and pay off less.
No namespace wraps a group: every name in it is minted, so nothing collides, and WGSL declares the same names at file scope too.

## EMIT-87

In a descriptor set, `-fvk-use-dx-layout` would give a constant buffer dx12's layout by itself, and slib passes it.
The offsets are stated anyway, so the text says where each member sits whatever flags its compiler was given, which is EMIT-4.

## EMIT-94

Vulkan and WGSL read a vertex attribute by its location, and sg's vulkan backend gives attribute i location i, so the order of the attributes is the one contract every target shares.
Splitting a vertex input over several buffers is a host question — which buffer holds which member, and how it steps — and the shader reads one struct either way.
So the text ignores streams entirely, and the host layout lists its attributes in member order whatever buffer each comes from.

## EMIT-95

A target's compiler reflects the names the text declares, and those follow each target's identifier rules and reserved words.
The host binds by the path instead, so whoever compiles the text renames what the compiler reflected, and needs the pairs to do it.
slib does that as the compile settles, which is why no target's identifier rules ever reach sg.

## EMIT-135

WebGPU has no ray query, and a portable shader that traces is worth more than one refused on a whole platform.
So the trace is SGL source the prelude holds twice, a native and an emulated form, and each target writes the one it can.
The emulated form reads one pool per context rather than a binding per structure, since a traversal walks from a TLAS into BLASes nothing else binds.
The acceleration member keeps its slot and takes no binding, so every other member's slot is what it is on the other targets, and one layout serves all four.

## EMIT-137

DXR's payload qualifiers are a promise the driver optimizes by: a field nobody writes after the caller need not travel back.
Every shader of one pipeline must state the same qualifiers, so they are inferred from the whole module rather than per entry point.
A host's hit group is compiled apart and cannot see the module, so a payload it may meet states the widest access, which DXC reads as a missed optimization and not an error.

## EMIT-138

A struct member named like a type, `ray: ray`, is legal in SGL, where a member lives in its struct's scope.
In HLSL and MSL the member's declaration hides the type for the rest of the struct, so a later member of that type fails to compile.
Renaming on every target keeps one member name across the texts, which is what a reader comparing them expects.

## EMIT-139

Metal has no shader table: a kernel intersects with its own intersector and calls what it found through function tables.
One table holds the miss and closest-hit functions of every ray type, so they share one signature, and the payload travels as words each function reads as its own type.
DXR finds a closest hit's record from the instance's hit-group offset, which Metal's intersection result does not carry, so sg binds each instance's offset beside the tables.
A procedural group's intersection and its any hit are one function on metal, since Metal runs no any hit after a box's intersection, which is why CHK-345 fuses them.

## EMIT-142

Every target swizzles its own vectors, spelled the same, so the text reads like the source and like hand-written HLSL.
Lowering every swizzle to a construction in the check pass was the alternative: nothing downstream would change.
It would also grow a temporary and a constructor at most swizzles of ported code, and leave a write through a swizzle no node to stand on.
A struct of the program is a struct in every target and no vector, so it keeps the construction.

## EMIT-148

A text is per target, never per device (EMIT-13), so dx12's has to be right on every device, NVIDIA's included, which runs 32 alone.
The exact form, `[WaveSize(64)]`, fails the pipeline on such a device; the range form runs everywhere and prefers `n` where it can.
SM 6.8, which the range form needs, is already the profile SGL's DXC edge compiles.

## EMIT-149

The value is uniform only if no thread stores to `m` between the first thread's read and the last one's.
The barrier ahead of the read publishes what was stored before it, and the one after it keeps every later store behind every read.
Tint writes WGSL's own load for HLSL the same way.

## EMIT-154

A host struct written with no knowledge of HLSL places a `float3` in 12 bytes and starts the next value right behind it.
HLSL's constant buffers read in rows of 16 bytes, so a block laid out that way needs every member placed by hand there, and split where a vector crosses a row.
That is the cost the `.cpp` layout asks for, and it is why `.hlsl` exists beside it.
