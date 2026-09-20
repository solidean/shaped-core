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
