# Why: emitting

*Tracer: deliberately thin.*

The reasons behind the rules of [emitting.md](../emitting.md).
Nothing here is normative.

The emitters follow the [compilation model](../../incubator/compilation-model.md): SGL compiles to shader text, and the text's own compiler optimizes it.
So an emitter is a printer with opinions about names and addresses, and it transforms nothing.

## EMIT-2

One HLSL text for both APIs would need something after it that numbers the bindings, which is the pass SGL exists to make unnecessary.
The two differ in little: where the inline constants live, and that SPIR-V has locations where dx12 has semantics.
That little is exactly what a reader of a capture wants to see, so each target says it in the text.
Both share one dialect in the implementation, and a fourth target is one more file.

MSL came as that one file: a dialect, a reserved-word list and a case, and the walk over the tree did not change.
What it did change is a rule every target shares, since MSL's block layout joined the comparison of EMIT-41.

## EMIT-4

A matrix's orientation in HLSL can come from `-Zpr` or from a `#pragma pack_matrix` that the text cannot see.
Either one would transpose the cube with every size still correct, and nothing would report it.
So every matrix member says `column_major`, and the product is written `mul(m, v)`, which means the same under either flag.

## EMIT-6

The stages of a pipeline are compiled apart by every API, and hot reload replaces one of them.
A text that needed its sibling would make the pair the unit, and a vertex stage is reused with several pixel stages.
What the two share is the stage link, and its addresses come from member order alone, so both stages compute the same ones without talking.

## EMIT-13

A shader that compiles for dx12 and fails for the web is found late, by whoever builds the other platform.
An error that every target reports is found by whoever wrote the line.
So a rule one target needs is a rule of the language: a layout WGSL cannot express is refused for HLSL too.

## EMIT-21

The host asks for an entry point by name, so its name cannot change per target the way a local's can.
Were `filter` legal as an entry point, it would be `filter` on two backends and something else on the third, and the host would need the table.
An error that names the targets is the smaller surprise.

## EMIT-40

DXC packs a push-constant block tightly, whatever `-fvk-use-dx-layout` says, so `{float3; float; mat4}` would sit elsewhere than on dx12.
Nothing reports the difference: both modules compile, and the shader reads its members from the wrong bytes.
An offset on every member makes SPIR-V agree with the one layout the host fills, and it is there for the reader too.
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
A struct called `filter` or `length` would then be ambiguous at its first use, and the compiler would name a header nobody wrote.
A local only hides such a name, so reserving it there costs an underscore and nothing else.
`main` is no keyword, and MSL refuses a function of that name, so by EMIT-21 no target has an entry point called `main`.

## EMIT-58

MSL has no global resources: whatever a stage reads is a parameter of its entry point.
The body still reads `constants.view_projection`, since a reference parameter is used like the global the other targets declare.
sg's metal backend binds group N at buffer index N and keeps four such slots, so 4 is the first index no group can take.
That backend sets no inline constants yet, so the number is a proposal it has to adopt, and the text is where it is written down.
A vertex buffer has no index in the text at all: `[[stage_in]]` reads through the pipeline's vertex descriptor.

## EMIT-62

MSL's `float3` is sixteen bytes, so the `float` that HLSL and WGSL put into its tail starts a new row in MSL.
`packed_float3` is twelve bytes and would agree, but it is another type, and every read of it would want a conversion.
Refusing the block keeps one host struct for every backend, which is what EMIT-13 asks for.
The layout of a vertex input is not the struct's: `[[attribute(i)]]` reads through the vertex descriptor, so EMIT-62 is about blocks only.

## EMIT-63

MSL is column-major with the vector on the right, as WGSL is, so there is no `mul` and nothing to state about orientation.
It could build a struct with braces, `pixel_input{a, b, c}`, which drops the member names just as a positional constructor would.
`const` stands in front of the type because the hand-written MSL in this repo writes it there.

## EMIT-64

An unsuffixed literal is a `double` in C++, and MSL has no `double`, so the question is fair.
The hand-written MSL in this repo and the MSL SPIRV-Cross generates both write `0.5` for a `float`.
So the unsuffixed form stays until a Metal compiler says otherwise.
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
