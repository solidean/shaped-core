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

## EMIT-110

The rule is chosen for what a GPU reads fastest, so a shader that names no layout is fast on every target.
Three rules were weighed: C's, where every value aligns to its scalar; std430's, where a vector aligns to its size; and HLSL's rows everywhere.

**In a buffer, a vector's alignment decides how it loads.**
NVIDIA loads 16 bytes in one instruction only from a 16-byte boundary, so a `float4` the compiler cannot prove aligned is four 4-byte loads.
In a kernel bound by bandwidth that is four times the load instructions: the threads still coalesce into the same cache lines, so it costs issue and latency rather than DRAM traffic.
Apple's wide loads want natural alignment too; AMD's buffer loads need only 4 bytes, so they barely care.
WGSL aligns a `vec4f` to 16 in storage and cannot lower an alignment, so under C's rule a `float4` at byte 4 would be four scalars in its memory form, slow even where the data happened to be aligned.
That is why std430, WGSL's storage rule and MSL all align a vector to its size, and a 3-vector to 16.

**In a constant block, a vector crossing a 16-byte row costs a read.**
HLSL reads a constant buffer by rows, so under C's rule a `float4` at byte 4 is two row loads and a shuffle.
NVIDIA reads constants per scalar operand, so it barely matters there, but it is not free on every target.
Under the natural rule no vector crosses a row, so HLSL's constant buffers, std140 and WGSL's uniform rule all read every scalar and vector natively.

**C's rule packs tightest, and rows everywhere packs loosest.**
HLSL's rows in a buffer would stride a `float[N]` by 16, four times the memory and the bandwidth, which is no default to want.
The natural rule costs a few padding bytes before a vector, which the generated host struct spells out as named fields.

So SGL is fast by default, and the cases where the natural rule and a C++ programmer's intuition disagree are the ones `@padding` names explicitly.
`@padding(.none)` is C's rule, for a host struct written by hand or memory something else reads, such as the vertices a BLAS is built from.
A `mat3` (48 bytes, or 36 packed), an array of 3-vectors (a stride of 16, or 12 packed), and an array of scalars in a constant block are the others; they land with arrays and matrices in GPU memory.

## EMIT-112

One C++ struct copied into what every target reads needs one layout, and the targets' own rules disagree on it.
The natural rule is WGSL's own and nearly MSL's, so the rule costs those two least: WGSL writes every root as it is, and MSL writes a memory form only where a value sits in a 3-vector's tail.
HLSL is the target that pays, with a padding field in each gap: a structured buffer packs as C++ does and a constant buffer packs within its rows, and both close every gap a value fits into.
Filled gaps land every member at its offset, since no vector crosses a row and nothing else moves.
The padding stands in the struct's declaration rather than in a flattened memory form, since HLSL builds a struct value member by member anyway.
So the padding is invisible to the program, and every struct stays readable.
A struct an HLSL text pads is therefore padded in function bodies too, and DXIL refuses to store a field it cannot prove defined, which is why such a local is zeroed first.
A buffer of bare 3-vectors cannot carry padding in a struct of the program, so it is the one root HLSL writes as a memory form under the natural rule.
`scalarBlockLayout`, which sg's vulkan backend requires, admits every offset the natural rule and `@padding(.none)` produce, and nearly every Vulkan device has it.

## EMIT-169

HLSL strides an array of `float3` by 12 in a structured buffer, where every other target strides it by 16, so one target needs the padding spelled.
A wrapper struct per element would read as `a[i].value`, where a 4-vector reads as `a[i].xyz`, the swizzle any reader of HLSL expects.
A constant buffer already starts each element on a row, so the 4-vector is its layout too, and one spelling serves both.

## EMIT-170

MSL's `float3` takes 16 bytes, so a scalar that SGL places in its tail would land 4 bytes later in MSL.
A memory form flattens a root to fix that, which cannot reach a struct repeated in an array, and the array of lights is the common case.
Declaring the vector packed fixes the struct itself instead, the way HLSL's padding fields do (EMIT-112), so it holds wherever the struct stands.
A packed vector converts to its plain type on assignment, but MSL's builtin functions take only the plain one, which is why every read converts it.

## EMIT-172

HLSL's constant buffers start each array element on a row, WGSL's uniform rule requires a stride of 16, and MSL strides by the element alone.
An array of 4-vectors is the one declaration every target places alike, and reading `.x` of it is what a hand-written port of such a shader does.
The attribute stands on the array, so its cost, three unused lanes per element, is visible where the array is declared.

## EMIT-178

`@padding(.none)` promises the bytes of a C++ struct, and a C++ array of `tg::vec3f` strides by 12.
HLSL's structured buffer already places every value at its scalar's alignment, so only WGSL and MSL need a form, and the form already existed for single values.
An array of scalars is the one declaration both targets index at a runtime offset without imposing a stride of their own.
So a split array keeps every element's scalars in one field, rather than one field per scalar a runtime index could not choose between.
MSL's `packed_` vectors stride by their scalars, so it splits only matrices, which have no packed spelling.

## EMIT-177

No machine that tests SGL runs Metal: the macOS runner has no Metal 4 device, so the metal backend's suite is skipped there.
The Metal compiler does run there, on every shader a package builds, so a layout it can check at compile time is checked on every push.
A wrong offset otherwise reads wrong values without an error, which is how a constant block's struct sat at the wrong byte unnoticed (EMIT-176).
HLSL and WGSL need no such check: DXC's and WGSL's own reflection already confirm each offset in the cross-compiler layout tests.

## EMIT-175

A `mat2` is 16 bytes in every buffer layout WGSL, MSL and a structured buffer agree on, and C++'s `tg::mat2f` is the same bytes.
Only HLSL's constant buffer disagrees, taking a row per column, so a block holds the two columns in one `float4`, as a hand-written port does.
Aligning it to a row in a constant block keeps that `float4` inside one row, which HLSL requires of every vector there.
A struct holding one then has one size in a block and another in a buffer, which EMIT-114 already reports as two layouts.

## EMIT-174

A location holds one vector of up to four lanes on every target, so a member per column or element is the declaration that means the same thing everywhere.
Packing two short pieces into one location would save locations but change what `@interpolate` applies to, and the limit of 16 or more has not been reached by any shader yet.
The rebuild costs nothing a compiler cannot fold, and a literal index reading its piece alone keeps the common case free of the local.

## EMIT-173

The text writer once worked out how to spell a member from where it was writing it, through flags it set while writing a place or indexing an array.
A flag set a node too early or too late wrote the wrong text silently, and the read conversion that landed on an assignment's left side in MSL was exactly that.
A table filled before writing has no such window: each expression carries what it is held as and whether it is read as a value, and the writer looks both up.
New flat nodes for loads and stores would say the same, at the cost of a case in every pass over the flat tree for nodes that only an emitter sees.

## EMIT-168

HLSL places a `float3x3` differently in each memory: 44 bytes in a constant buffer, each column on a row but the last short, and 36 in a structured buffer.
Neither is the natural 48, and a memory form would flatten every struct holding one, which HLSL cannot rebuild into a struct value.
A `column_major float4x3` is exactly the natural layout in both: three columns of four floats.
Its upper-left 3x3 is the matrix, which is what HLSL's cast to `float3x3` reads, and a constructor from the matrix's three rows and a zero row writes it.
So the matrix stays a field of its struct, and the cost is a cast at each read and a call at each write.

## EMIT-171

DXC decorates a struct member's matrix with its stride and its majorness through one array at most.
A `float4x3 turns[2][2]` reaches SPIR-V with neither, so vulkan reads its last column as zero, while DXIL needs no decoration and is right.
A matrix in a struct of its own is a member again, at any depth, and the struct adds no byte, so both targets hold one form.

## EMIT-116

Reordering members is how a compiler packs a struct tighter, and it is planned: a `{float; float4; float}` block takes three rows where `{float4; float; float}` takes two.
Promising declaration order now would make that a breaking change for every host that computed an offset by hand.
The generated struct already follows whatever the compiler chose, so a host that uses it loses nothing.
A host that needs a fixed layout, such as memory shared with a hand-written struct, asks for one by annotation, `@padding(.none)`, rather than by accident.

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

HLSL takes the emulated form only by request, because a device's hardware must not decide how fast a shipped trace runs.
A choice made per device would make the same build slow on some machines with nothing in its source saying so.

## EMIT-137

DXR's payload qualifiers are a promise the driver optimizes by: a field nobody writes after the caller need not travel back.
Every shader of one pipeline must state the same qualifiers, so they are inferred from the whole module rather than per entry point.
A host's hit group is compiled apart and cannot see the module, so a payload it may meet states the widest access, which DXC reads as a missed optimization and not an error.

The validator and state object creation both accept a native module that states no access, and it once left the annotations out.
NVIDIA's driver does not: two pipelines of one device that disagree on a payload's access lose the second one's payload writes.
sg's text and native routes run on one device, so the native writer states what DXC states.
[nvidia-reorder-payload-qualifiers-across-pipelines](../../../../../../../docs/bugs-external/nvidia-reorder-payload-qualifiers-across-pipelines/readme.md) has the reproduction.

## EMIT-138

A struct member named like a type, `ray: ray`, is legal in SGL, where a member lives in its struct's scope.
In HLSL and MSL the member's declaration hides the type for the rest of the struct, so a later member of that type fails to compile.
Renaming on every target keeps one member name across the texts, which is what a reader comparing them expects.

## EMIT-139

Metal has no shader table: a kernel intersects with its own intersector and calls what it found through function tables.
One table holds the miss and closest-hit functions of every ray type, so they share one signature, and the payload travels as words each function reads as its own type.
DXR finds a closest hit's record from the instance's hit-group offset, which Metal's intersection result does not carry, so sg binds each instance's offset beside the tables.
A procedural group's intersection and its any hit are one function on metal, since Metal runs no any hit after a box's intersection, which is why CHK-345 fuses them.
The motion tags double a static scene's traversal on an M4, nearly all of it `instance_motion`'s, so only a compile that may be bound a structure that moves takes them.
That is a package with `SOFTWARE_TRACE`: sg refuses a moving structure to every other, so a trace without the tags never meets one.
One macro rather than a flag per site keeps every function of a pipeline alike, since each is compiled apart and must link.

## EMIT-142

Every target swizzles its own vectors, spelled the same, so the text reads like the source and like hand-written HLSL.
Lowering every swizzle to a construction in the check pass was the alternative: nothing downstream would change.
It would also grow a temporary and a constructor at most swizzles of ported code, and leave a write through a swizzle no node to stand on.
A struct of the program is a struct in every target and no vector, so it keeps the construction.

## EMIT-148

dx12's text has to be right on every device that runs it, NVIDIA's included, which runs 32 alone.
The exact form, `[WaveSize(64)]`, fails the pipeline on such a device; the range form runs everywhere and prefers `n` where it can.
The range form is Shader Model 6.8's, past the 6.6 a host may target (EMIT-264), so a host targeting 6.6 asks the `none` fill of `wave_size`.
A preference is only a hint then, which the host still has beside the text.

## EMIT-265

What a shader model adds is often what a device has some other way, so a target's form of a construct is a choice beside whether the device has it.
NVIDIA's devices reorder and see micromaps through NVAPI at 6.6, and a draw's start can arrive in a root constant, so a fill names that choice per construct.
A feature stays what the device can do, and its fill how this compile writes it, which keeps the features one list across every target.

## EMIT-149

The value is uniform only if no thread stores to `m` between the first thread's read and the last one's.
The barrier ahead of the read publishes what was stored before it, and the one after it keeps every later store behind every read.
Tint writes WGSL's own load for HLSL the same way.

## EMIT-154

A host struct written with no knowledge of HLSL places a `float3` in 12 bytes and starts the next value right behind it.
HLSL's constant buffers read in rows of 16 bytes, so a block laid out that way needs every member placed by hand there, and split where a vector crosses a row.
That is the cost `@padding(.none)` asks for, and why it is no default.
Every rule places the same, ordinary C++ struct, since every generated host type aligns to its scalar: `tg::vec3f` is 12 bytes at 4, `tg::mat4f` 64 at 4.
So the rule is EMIT-167's, and a struct a packed block holds would have the layout it has in a packed buffer.
`packoffset` was the first plan for dx12, and it cannot carry the rule.
It counts in 4-byte components, so a `half` at byte 2 has no spelling.
It stands only in a `cbuffer`, whose members are globals rather than fields of the `ConstantBuffer` every other block is read through.
Neither is needed: once no field crosses a row and a padding field fills each gap, dx12's own packing puts every field right behind the one before, which is where the form placed it.
A memory form on every target, rather than only where a target's own rule disagrees, keeps one shape of text for a promised layout.

## EMIT-161

WGSL and MSL name only a weak compare-exchange, so SGL offers the weak one as it is and builds the strong one on it.
Both are named for what they promise, so no call reads as one while meaning the other.
The weak one has to say whether it stored, since a spurious failure reads the expected value and stores nothing.
WGSL's own result is a struct no program can name, and the WGSL text writes every local's type, so `exchange_result[T]` is SGL's own and every target fills it.
HLSL's `InterlockedCompareExchange` is strong, and DXC writes SPIR-V's `OpAtomicCompareExchange`, which is strong too; strong satisfies weak, so HLSL needs nothing more.
SPIR-V's own `OpAtomicCompareExchangeWeak` is deprecated, and "has the same semantics as `OpAtomicCompareExchange`".
WGSL says "the `exchanged` member of the result structure may be `false` even if the `old_value` member equals `cmp`" (§ 17.8.5).
MSL describes its `atomic_compare_exchange_weak_explicit` by the strong effect alone (§ 6.16.4.4), but its name and C++ allow the spurious failure, so SGL assumes it.

The strong loop retries only a failure that read `expected`, so a real conflict, which read anything else, ends it at once.
No invocation ever waits there for another to do something, which is what deadlocks a spin lock in a SIMD group whose lanes cannot progress apart.
Each lane runs its own attempts and leaves when its own attempt says so, so lanes leaving at different iterations only reconverge after it.
It is lock-free rather than wait-free: a device may in principle fail a lane spuriously again and again, and no spec bounds it.
Tint and Naga write WGSL's weak one as the strong instruction on D3D and Vulkan, so only Metal could fail it spuriously, and its spec describes no such failure.
So the loop almost always runs once.
In WGSL a `break` is its only exit, so its behavior is {Next}, and the control flow after it is the control flow before it (§ 15.2.6), which keeps a barrier after the call legal.
MSL and HLSL have no static uniformity rule, and every thread leaves the loop, so a barrier after it is reached by all.

## EMIT-162

A helper invocation of a pixel stage runs for derivatives, or goes on after a `discard`, and its atomics store nothing.
What they read is no value at all: Vulkan says "values returned by atomic instructions in helper invocations are undefined", and WGSL that they "return indeterminate results" (§ 15.4).
D3D's helper pixels read 0 from an atomic that returns a value, and a pixel after its `discard` reads an undefined one (D3D11.3 functional spec, § 16.10 and § 16.13).
MSL says helper threads' "stores and atomic operations [...] have no effect on the underlying memory" (§ 6.11.1), and nothing about what they read.
So in a helper the strong loop's test, a failure that read `expected`, may hold forever, and a loop that never ends hangs the GPU.
MSL tells a helper by `simd_is_helper_thread()`, so the loop skips one, and gives `expected`, as meaningless as any value a helper reads.
HLSL's strong compare-exchange is one instruction, so it needs neither; SM 6.6's `IsHelperLane` would be the test if it ever did.
A compute stage has no helpers, so its loop needs no test.

## EMIT-163

WGSL has no test for a helper invocation, so its strong loop cannot be kept from one.
Bounding the loop would end it, at the price of a strong compare-exchange that may give `expected` without having stored, which is the weak one's promise under the strong one's name.
So WebGPU refuses the pixel stage instead, and the shader says what it means.
A weak compare-exchange in a retry loop of its own puts the loop in the shader's hands, where a helper's meaningless result is the shader's to handle.
A compute stage has no helpers, so it takes the strong one on WebGPU as everywhere.
The refusal is a feature, `helper_lane_query`, rather than a rule of the WGSL emitter, so it holds on every target, as EMIT-13 asks.
A pixel stage without the `require` is refused on the dev box at its line, and one with it is refused on WebGPU by name.
Once WebGPU can tell a helper, sg reports the feature there and nothing else changes.

## EMIT-219

The bodies hold on dx12 because D3D states its double arithmetic exactly.
The D3D11.3 functional spec's "64-bit (Double Precision) Floating Point" says "double-precision instructions match IEEE 754R behavior requirements".
Its one exception is the reciprocal, "permitted 1.0 ULP tolerance and the exact result if representable", which the bodies never take.
Its instruction list names `dfma` "double precision fused multiply-add", and the section says double instructions honor a subnormal rather than read it as zero.
So each addition, multiplication and `fma` the bodies take is correctly rounded, and the residual `sqrt` corrects by is exact, which is what bounds it to an ULP.
The rounding bodies clear bits, which needs no arithmetic to be exact at all.

## EMIT-256

vulkan gives the bodies nothing to stand on.
The Vulkan spec's "Precision and Operation of SPIR-V Instructions" says "The precision of double-precision instructions is at least that of single precision".
That covers the `OpFAdd`, `OpFMul` and `Fma` the bodies are made of.
Its table states `fma()` as "Inherited from OpFMul followed by OpFAdd", so a vulkan `fma` need not round once, and the residual `sqrt` corrects by may itself be wrong.
So on vulkan the bodies would be slower and no better bounded than the driver's own instructions, and their correction is not even sound there.
The same table states `round`, `roundEven`, `trunc`, `floor`, `ceil` and `fract` as "Correctly rounded", `inversesqrt()` as 2 ULP, and `sqrt()` as "Inherited from 1.0 / inversesqrt()".

The choice is by API rather than by route, since that is where the instructions differ.
`target_api` already tells the two apart, and both vulkan routes end as SPIR-V, so writing the same instructions from each keeps them computing alike.
DXC hands a double to its own `floor` or `sqrt` as a float even for SPIR-V, an `OpFConvert` on either side of the float instruction.
Its inline SPIR-V, `[[vk::ext_instruction(8, "GLSL.std.450")]]`, takes the double as it is, which is the device of EMIT-225.
