# Why: checking

*Tracer: deliberately thin.*

The reasons behind the rules of [checking.md](../checking.md).
Nothing here is normative.

The pass as a whole follows the [compilation model](../../incubator/compilation-model.md): name resolution, type checking and evaluation cannot be separated.
Overloading is by type, so even an unqualified name needs types to resolve, and a type expression may need evaluation.
So there is no resolver that runs first and no tree of resolved names: there is one demand-driven pass.

## CHK-2

Placing the prelude in front needs no multi-file compilation.
One unnamed module is the smallest thing that holds several files, and it is what a module will be once several files declare one.
Concatenating the texts would have been less code, and every span of the program's file would then be off by the length of the prelude.
A position is the whole name of a file, since nothing else about it is known to the pass: a caller keeps the names, and the program's file is always the last one.

## CHK-7

One mistake is one diagnostic.
A name that does not resolve makes its `let` untyped, and every use of that local would otherwise report a mismatch of its own.
The reader fixes the first line and the rest vanish, so the rest were noise.
A constructor call keeps its struct type even with a bad argument for the same reason: what the call is was never in doubt.

## CHK-8

The tracer carries one program, and the language is far larger.
A construct outside it has three possible fates: a crash, a guess, or a diagnostic.
A guess is the dangerous one, because it compiles, and a shader that compiles to something else is found on screen and not in the editor.
One kind for all of it keeps the list of what is missing searchable: every `unsupported-yet` in the source is a line of the roadmap.

## CHK-17

A pipeline of passes needs an order of declarations, and an unordered scope has none.
Demand gives the order for free: a struct's field asks for its type, and that type is compiled from inside the struct.
The four states are written so that "in compilation by somebody else" can later mean "await it", which lets one module compile wide.
With plain recursion, as today, the same state can only mean a cycle.

## CHK-20

A body is needed by a caller for two reasons only: to infer a return type and to inline the call.
Inlining reads the side tables of a body that was checked on its own, so it demands nothing.
Inference does demand the body, and only of a function that asks for it, by CHK-135.
Checking every other body after the signatures keeps an overload set usable from inside one of its own members.
With a demanded body that would be a cycle, since resolving the name asks for every candidate.

## CHK-30

The alternative to a name is an id in the attribute, `@builtin(normalize_vec3)`, and it says everything twice.
A name keeps the prelude readable as the documentation of the builtin functions, which is its second job.
The parameter types are part of the key because an overload is what differs: `dot` on `vec3` and `dot` on `float3` evaluate alike and are still two records.
Keying by name alone made every overload need a name of its own somewhere in C++, and that second list is what the registry removed.
The registry reads its keys back from its own text, through the parser, so a record states its signature once.

## CHK-37

An operator function needs a name because every function has one, and `transform_position` documents what `mat4 * pos3` means.
If that name were in scope, the prelude would take dozens of plain words away from every program: `add`, `multiply`, `scale`.
Hiding it costs nothing, since the operator is the better spelling of the same call.

## CHK-43

A binding is a global of the target, or a local function that is inlined, so there is never an argument to pass.
The list is an effect annotation: it says what a function may touch, and the check is that it touches nothing else.
The [binding effects](../../incubator/binding-effects.md) idea has the second, later half: that every callee's list is satisfied.

## CHK-53

`let x = x * 2.0` is how a value is refined step by step without a `mut`, and without inventing `x2` for every step.
Rust made the same choice for the same reason, and a shader is mostly such chains.
Every local is a local of its own, so the flat tree never sees a name twice: the emitter mints `x`, `x_1`, … as it mints every name.

## CHK-54

`let length = length v` is the same refinement CHK-53 allows, and the prelude holds enough common words that forbidding it would forbid the idiom.
An ordered scope cannot be ambiguous: the newest declaration is the one in effect, so there is nothing to report.
One namespace keeps that rule whole — a local does not hide a name in one position and leave it visible in the next.

## CHK-188

A new function or type in the prelude must never break a program that already used its name.
With one shared scope, every prelude addition would be a `duplicate-declaration` somewhere.
Only a clash inside one unordered scope is an error, since there no order says which declaration is meant.
Overload sets still join across the two, so declaring `dot` for a struct of the program's own adds to the prelude's `dot` rather than hiding it.

## CHK-192

Joined overload sets would otherwise reopen CHK-188's problem for functions: a prelude that gains a signature the program already declares makes every call of it `ambiguous-overload`.
The rule is special to the prelude because the prelude is special: nobody writes it into the program, it is placed around every file implicitly.

## CHK-110

An inner block shadows by CHK-53, the way the same block does, and what it declares is gone behind it.
Two blocks beside each other share nothing, so the same name in both is fine: that is no shadowing, and guard clauses need it.

## CHK-115

`..=` would be `end + 1` in the flat tree's `for`, which wraps where `end` is the last `int`.
A rule that is wrong for one value is no rule, so the closed range waits for a flat `for` that can say it.

## CHK-116

A function evaluates its arguments before it runs, and `a and b` must leave `b` alone where `a` is false.
So no `@operator` function can stand behind the two, and `not` joins them because the flat tree has a node for it.

## CHK-124

`while 0 < 1:` runs forever, and the rule still says that what follows it is reachable.
A rule that looked at the condition would make a program's validity depend on what the compiler can prove constant, and that moves with every release.
`loop:` is how a program says "forever", and the rule takes it at its word.

## CHK-129

The language's model was that a body is checked where it is inlined, since a generic has no types before that.
Generics arrived the other way: a type parameter is opaque, so a generic body means the same at every call too, and is checked once (CHK-338).
Checking once is what reports an error in a function nobody calls, and what reports it one time where three calls would report it three times.

## CHK-131

A binding is a global of the target, so a callee reads it without being handed it.
The list on the caller is what keeps that visible: an entry point's list names everything its shader touches, and the pipeline layout follows from that list alone.
The check is per call and needs no walk: each caller answers for its callees, so the chain closes by induction.

## CHK-72

Without implicit conversions a match is exact, so two matching candidates have the same parameter types.
That could be reported where the second one is declared.
It is reported at the call because a declaration's parameter types are only known once it is compiled, which a call demands and a declaration does not.
Once conversions exist, ambiguity at the call is the rule that is needed anyway.
With conversions, a best candidate is needed as well: CHK-254 ranks by chain length, and two candidates that each win somewhere have no best.

## CHK-243

A default is a small prologue of its function: it runs where the call stands, once per call that leaves its parameter unfilled.
But it is written in the function, and it reads what the function's scope reads.
Resolving it where the call stands would make `shade(n)` mean something different in every file that calls it, and none of that would show in the signature.
Checking it once at the declaration is what lets a call bind without checking an expression per candidate.

## CHK-247

Dot and free calls collect the same candidates so that the spelling is a choice of style and nothing else.
A generic function then never has to prefer `a.foo()` to stay general, and no API is reachable only one way.
Searching the first argument's type scope is argument-dependent lookup restricted to one argument, which is what keeps the set small enough to predict.

## CHK-251

Naming an argument for the reader's sake should cost nothing, so `make_light(color = c, 2.0)` binds.
A positional argument whose parameter depends on the names written before it is what the rule excludes: `make_light(intensity = 2.0, c)` would bind `c` to whichever parameter was still empty.
The strictest rule, positionals before any name, would reject the first call, and relaxing it to this one later would change nothing already written.

## CHK-254

A single number per candidate, the sum of its chains, would let a candidate win by being much better at one argument and worse at another, a trade the reader never asked for.
Dominance resolves a call only where one candidate is at least as good everywhere, and every call it resolves a sum resolves the same way, so relaxing it later breaks nothing.

## CHK-256

The spelling says what the writer means: a property is read, a function is called.
It takes no part in resolution, so the error always names the fix rather than reporting that nothing was found.
A free call may reach a property so that generic code can write `length v` for anything that has a length, whether it is a property or a function.

## CHK-62

A bare name inside a method was first read through `self` too, and that put a field of the receiver between a local and every name of the module.
Every lookup that starts from a name then had to know about the receiver, and two of them did not: `frame.x` in a method found a binding `frame` before a field of that name.
Writing `self.frame.x` makes the receiver a name like any other, and a body reads the same wherever it is moved.
A field's default is the exception that is none: it reads the constructor's parameters, and no receiver exists while they are bound.

## CHK-253

A literal leaving its default type is one step of its chain, so dominance alone ranks calls of literals.
Free conversion with a count of the literals kept at their type broke ties by summing over the arguments, which CHK-254 rejects for chains.
`f(1, 2, 3)` would then pick the candidate that keeps two literals over one that keeps the third.
With the step, `f(1)` still picks `f(x: int)` over `f(x: float)`, and a call each candidate wins somewhere is ambiguous.

## CHK-257

An operator over literals alone meets whatever operators the prelude declares, and a prelude that adds one of the literals' own type would change the answer.
`7 / 2` is `3.5` through the float `/`, and `3` through an int one.
The call names no type, so no answer is the reader's, and the error asks for one.
Folding literals in the front end would settle it for good, as the [literal-types](../../incubator/literal-types.md) incubator sketches, and nothing written under this rule changes meaning then.

## CHK-269

A hex literal is how a mask or a bit pattern is written, and a mask that means one number as a `uint` and another as an `int` is the surprise to avoid.
Reading it as the number it spells is CHK-253 as it stands, so no rule of its own decides what it converts to.
A pattern with the top bit set belongs in a `uint`, or is built with `int.from_bits`.

## CHK-313

`/` and `%` are where an integer answer and a float answer differ, and a reader of `1 / 3` cannot tell which the writer meant.
Folding literals, which the [literal-types](../../incubator/literal-types.md) incubator sketches, makes that expression an error rather than a silent `0`.
Refusing it now means nothing written today changes meaning when folding lands, which is CHK-257's reason carried over to the day `int` gained a `/`.

## CHK-81

A literal converting by a call of the type's name gets defaults, named arguments, named-only parameters and the evaluation order from the call rules.
There is no second set of rules for making a struct from values.
Every function of the name takes part, so a program that adds `fun ray(.from: pos3, .to: pos3)` can write `{from = p, to = q}` where a `ray` is expected.

## CHK-106

Whether a call has an effect decides what the compiler may leave out around it, and only the declaration can know.
A builtin is a name the compiler maps to something in each target, so its purity is a fact about the target's function, and the prelude is where such facts stand.
The default is the safe side: a builtin nobody marked keeps its place among its neighbours, which costs a local and never a wrong result.

## CHK-97

Full inlining is the language's model, so a target never sees a function of the program, a generic or a lambda.
An emitter that reads the AST would have to know all of them, five times over for five targets.
The flat tree is the narrow waist: whatever the front of the language grows into, an emitter reads the same few node kinds.
It keeps every AST node and every call site so that a position in the emitted text can still be traced to the source, through the calls it was inlined through.

## CHK-104

The emitted text is meant to be read, so a local keeps its name wherever it can.
Inlining puts the locals of many functions into one, and two of them called `n` must not meet.
A mint that every name passes through makes a collision impossible by construction, where a check after the fact could only find one.
The module-level names are taken first because a target has one namespace where SGL has two: a local `normalize` would hide the builtin it is about to call.

## CHK-135

A caller needs a signature, and the signature of such a function is not known before its body is.
So the body joins the demand, for these functions only.
Every other body is still checked after all signatures are known, which is what keeps an overload set usable from inside one of its own members (CHK-20).
The price is the one CHK-136 names, and it is the honest one.
Two functions that infer their results from each other have no result, and nothing short of a written type can give them one.

## CHK-137

A call may have been written for its effect, and a function with a value is no less callable for it.
Whether a PURE call is worth a statement is a question about the program, and the AST pass already asks it, as the warning `no-effect`.
Refusing here what the AST pass only warns about would make the two disagree.
Any other expression as a statement has nothing it could be for, so it stays refused until something gives it a meaning.

## CHK-138

The split is by who writes the file, which is also what each needs.
A builtin needs C++ behind it: an evaluator, a spelling per target, a layout.
So C++ is the source of truth, and the file is generated from it: one record per builtin, and nothing that has to agree with it.
The generated file is committed all the same, since the prelude is the documentation of the builtins and a diff of it is how a change to them is reviewed.
`core.sgl` is what needs no C++: ordinary SGL that is checked and inlined like a program's own functions.
Until SGL has generics a program may declare, an overload family cannot be written once in SGL, so the registry's C++ loops write the families out and `core.sgl` holds next to nothing.
`raytracing.sgl` is a file of its own because it is large and has one topic: the trace, its vocabulary, and the software traversal webgpu runs.

## CHK-150

An enum that converts to `int` is an `int` with a nicer spelling, and the conversion is what makes it one.
Once `light_kind` can be added to 1, every guarantee the type was declared for is a convention rather than a rule.
The compiler stops being able to tell a case from a number that happens to be in range.
So the first version converts in neither direction, which is the position it is possible to relax from.
The relaxation is already named: `value as int` and `n as light_kind`, both unchecked, because a bounds test on every conversion is not worth its cost in a shader.
[enum futures](../../incubator/enum-futures.md) is where it waits.
Starting strict costs a cast somebody has to write; starting loose costs the type.

## CHK-154

A pattern language is the obvious thing to build here and the wrong thing to build first.
`scrutinee == pattern` is one sentence, it needs no new node family, and it subsumes everything the first version wanted.
A leading dot is an expression, a literal is an expression, and `_` is the one shape that is not.
It also decides the `int` scrutinee for free, which a pattern language would have had to decide separately.
What it gives up is exhaustiveness, which equality can derive only over a closed set — hence CHK-159's narrow rule.
[patterns](../../incubator/patterns.md) is the step after, where an arm destructures and binds, and where exhaustiveness becomes a real analysis rather than set membership.

## CHK-160

Two reasons, and they point the same way from different ends.

A `case` that is a value must produce one on every path, which is the property CHK-125 already asks of a function body; a scrutinee that matched no arm would leave it with nothing.
And WGSL refuses a `switch` without a `default` outright, so an emitter has to invent one whatever the language says.
Inventing one silently means a value the program never wrote, in the one place a reader would not look.
Asking the source for it costs a `_` and makes the missing case a diagnostic instead of a zero.

## CHK-171

A buffer's host name is what sg matches a bound view against, and what a shader package's generated table states.
No target binds by name: HLSL by register, SPIR-V by set and binding, WGSL by `@group` and `@binding`, MSL by index.
So the host name need not be an identifier of any target, and the path is the one name that is injective and needs no rule for a reader to learn.
`a_b.c` and `a.b_c` stay two buffers, where any identifier built from them would clash.
The identifier a target writes is the emitter's to mint (EMIT-85), and the text reports the pair (EMIT-95).

## CHK-197

SGL's semantics are the intersection of what the targets' fast native operations guarantee, and no operation pays a tax on every target to be defined where one target leaves it open.
Each target writes `x as int` as its own native conversion, and those agree only where the truncated value fits: SPIR-V, for one, leaves every other float undefined.
Defining saturation and a NaN of 0 would mean a clamp and a compare around every conversion on the targets that do not do it natively, in shaders where a conversion sits in the inner loop.
A program that needs a defined result clamps before it converts, and pays for it only where it asks.
The interpreter still has to give some value, and saturation is the choice that is right on the most hardware.

## CHK-225

A `bool` line is a check rather than a line under a `check` keyword, because the keyword would sit on every line of every test.
`assert` already covers the case that stops, so a second keyword would differ from it only in going on.
The cost is that what a statement means depends on its type, so the AST pass leaves `no-effect` in a test to the check pass (AST-140).
A call made for its effect that returns a `bool` becomes a check without saying so, which in a test is almost always what its author forgot to write anyway.

## CHK-226

The rule exists so a test cannot pass vacuously by mistake: a last line that computes and checks nothing is almost always a check its author meant to write.
EVAL-78's `no-check-ran` catches a run that checked nothing too, but only when the test runs; this rule says so when the test is checked, in the editor.
A test whose asserts are what it checks pays one line for it, `true // why`, which also says why it checks nothing else.
A test that expects `.fail` or `.assert` is exempt, since it cannot pass without its run failing: it is fail-closed already.

## CHK-317

A function value is a compile-time entity (the incubator's inferred-comptime idea), and the parameter is the one place it can be spelled so that every call through it is known statically.
A local or a field of function type would need a function value at runtime, a tag and a switch at every call, which breaks the performance contract without saying so.
Ray tracing is the first user: a trace takes its candidate decisions as functions, and the software traversal calls them inside its loop.

## CHK-315

A mut parameter is written on its type because that is where `mut` already stands for a geometry stage's stream, and for the access of a resource.
A `mut` on the name, `mut p: T`, would give one idea two spellings in one signature.
The ray-tracing stages are the first users: a payload is the caller's place, as DXR's `inout` is.

## CHK-316

Marking the argument keeps an effect on a variable visible where it happens, which the function model asks of every call.
An exact type is what a place needs: a conversion would make a temporary, and the callee's writes would land in it and be lost.
The indices of the place are evaluated once so that `bump(mut values[next()])` reads and writes one element, however often the body names it.

## CHK-259

A `require` is permission, and what an entry point needs is judged from its use (CHK-263).
So a file-level `require` costs nothing: a vertex stage in a ray-tracing file still runs on a device that cannot trace.
The alternative, where the file's `require` is every entry point's floor, would make the cheapest place to write it the most expensive one to have written.

## CHK-261

A binding is a layout the host binds whole, so what one of its members needs is needed wherever the binding is listed, read or not.
Its own `require` is therefore a requirement of the binding and not only a grant to its members: a library that ships a binding says, in the binding, what a device must have to take it.

## CHK-262

A file's `require` grants everything in its file, and declares only for that file's entry points.
A library whose file requires a feature says the file may use it, not that every shader listing one of its bindings must accept it.
The binding's own `require` is how a library says that (CHK-261), so a user's entry point is never made non-portable by a line in a file it does not own.
Letting a listed binding carry its needs to the entry point on its own would erase that difference, and a file-wide permission would silently become every user's requirement.

## CHK-263

The floor is defined by what the language counts, never by what an optimizer happens to remove.
A floor that followed dead-code elimination would change with a compiler version or an unrelated refactor, and the host would find out on a device that lacks the feature.
So every use the entry point reaches counts, including one behind a condition that is false at run time.
A branch on a constant is the one exception, and it keeps the floor the language's: CHK-356 removes it on every compiler alike, so the floor cannot move with an optimizer.
It is what lets an option turn a 16-bit path off without asking the device for the 16-bit feature.
A compile-time branch on a feature, `if feature ray_query:`, is the form that would leave a use out on purpose, and it is not built.

## CHK-265

A binding's `require` is how a library says what a device must have to take the binding, and a member need not be what uses it.
A binding that carries an acceleration structure later, or that a caller's shader reads through a feature, states the need before anything in SGL can show it.
So only a body's `require` can be unused: it says nothing about any binding, and it is the one place an unneeded line is certainly a mistake.

## CHK-271

An entry point's signature is its whole interface: what the GPU hands it in `()`, what it binds in `{}`.
That is what `sgl describe` and the pipeline check read, and what a reader looks at first.
A stage input as a field of the stage struct was the alternative, and it would have put a member no vertex buffer feeds into the struct the host mirrors as its vertex layout.
A builtin function such as `vertex_index()` is the other alternative, which a helper could call without being handed the id.
It may come later as sugar over these parameters, and the [stage-interfaces](../../incubator/stage-interfaces.md) incubator holds it.

## CHK-273

The two axes and their names are WGSL's, which are exactly what every target has: HLSL's qualifiers and MSL's attributes spell the same combinations.
An integer member must say `.flat` rather than being flat by default, because which vertex's value wins is part of what it means: a primitive id is right only because the first vertex's is taken.

## CHK-275

A format is not a type, which the vector-and-format incubator settled for render targets: a member has the type the shader computes with, and the format is an attribute.
A vertex member follows the same rule, so one word, `@format`, says "these bytes, read as this type" on either edge of a pipeline.
On a `@pixel struct` member it is that target's setting, and on a `@vertex struct` member it is the member's own, which is why it is never read as a setting there.

## CHK-276

A pixel stage's output is its return value, all of it, which is SGL's model for every stage; a depth written through a builtin call would be an output the signature hides.
`@position` is the precedent: a marked member of a stage's struct that the stage link treats specially.
A struct holding only `@depth` is how a depth-only pixel stage is written, which is what a shadow pass with a cut-out needs.

## CHK-277

A `discard` is a jump rather than a builtin call, since control flow reads as control flow: `if … => discard` ends that path the way `if … => return x` does.
A call is no jump, so a path that discarded would still have had to produce a value.
The portable meaning is that the pixel has no effect after it — no target, no depth, no stencil, no store — which every target keeps.
Whether the pixel keeps running as a helper for its quad's derivatives is where the targets differ, and the writers demote wherever a target offers the choice.

## CHK-285

`T[N]` reads the way a C, HLSL or GLSL reader writes an array, and `texture_2d[float4][64]` is a binding array by the same rule, with no second spelling.
Composed literally, `float[3][5]` would be five arrays of three, the reverse of what the same text means to those readers.
Making it mean the C order instead would break aliasing: with `type row = float[5]`, `row[3]` has to be three rows.
So several dimensions are one group, outermost first, and two groups in a row are refused rather than read one way or the other.
`length` names the count, since the texture methods already use `size` for texels and `count` would read as a binding's descriptor count.

## CHK-300

The uniformity pass already knows which index is non-uniform, so the compiler could insert the mark itself.
The author writes it instead, because it keeps the cost visible in the source.
On hardware that needs the mark, a marked access becomes a loop over the distinct indices in the wave.
A refactor that makes an index non-uniform then fails loudly, where an inserted mark would slow it silently.
The strict rule is also additively relaxable: making the mark optional later breaks no program, while hiding the cost now and asking for it back later would.

## CHK-320

The geometry is part of the type because it decides what a trace against the structure is written as and what it gives back.
A triangle trace hands back barycentrics, a procedural one the attributes its intersection reported, and a mixed one either.
Leaving it to the host would make the shader's result type depend on what was bound, which no target can compile.
It is required rather than defaulted to `.triangles`, since a default would silently skip every box of a structure that holds some.

## CHK-322

A builtin that needs a feature is reached through the prelude's functions, never named by the program: a trace is dozens of steps of the target's query.
So the use is counted where the entry point reaches the call, after inlining, which is the only place the chain from the entry point to the step is known.
The diagnostic names that call, so an author who wrote `world.trace(r)` learns which line needs `require ray_query`.

## CHK-331

A pipeline states what it names and derives what it could state wrongly.
The payload size, the attribute size and the depth are each a fact of the shaders, and DXR fails at run time, or silently, when a declared one is too small.
`max_recursion_depth` is the exception, and only beside `.host`: the host's shaders are compiled apart, so nothing here sees what they trace.

## CHK-332

The depth a pipeline needs is the longest chain of traces, which only a graph over ray types can bound.
A graph over shaders would be finer, and would need the table's indices at check time, which the host's rows make unknowable.
Over ray types, a cycle is a trace that can nest without end, which DXR allows up to a declared bound and SGL refuses.
True recursion is the [incubator's](../../incubator/raytracing-futures.md).

## CHK-338

A type parameter is opaque, so a generic body is checked once, over it, and an error in it is reported where it is written, whatever the calls.
The alternative, checking the body per instantiation as C++ does, reports an error at a call its author may not own, and reports it once per call.
Bounds would let a body ask more of `A`; nothing needed them yet, so a body may only hand a value of it on.
The first users are the prelude's traces, which carry an intersection's attributes of whatever type the program reports.

## CHK-339

A generic struct is the prelude's alone because the traces need exactly one shape of it, a hit or a report that carries the program's attributes.
A user-declared one raises questions nothing has answered yet: methods that name the type parameter, several of them, and what a host sees of an instance.
The [incubator](../../incubator/function-model.md) holds them.

## CHK-341

`report.none()` must build a report without attributes, and SGL has no optional type and no default value of an arbitrary type.
`undefined()` is that missing value, made safe by being the prelude's alone: the prelude never reads what it holds.
The interpreter reads it as zeroes so that a miss, which copies undefined attributes into its result, still runs in a test.

## CHK-342

DXR hands an intersection no payload, so SGL does not either, and an intersection's only output is its report.
The attributes are a struct of the program because every target passes them as a struct: HLSL's attributes parameter, and metal's words.

## CHK-343

A table belongs to the module rather than to a pipeline so that its place in the callable section is a constant of the module.
Were a table the pipeline's, a stage calling it would compile once per pipeline, with another offset each time, and a stage shared by two pipelines would be two shaders.
The cost is that every pipeline of the module holds every callable, which costs table space and nothing else.

## CHK-346

AMD's fast FSR build is its 16-bit path, and a port without 16-bit types runs everything in fp32, losing the packed-math rate and doubling workgroup memory and registers.
`half3` sits beside `float3` and `int3` as one family, which `f16x3` or `float16x3` would not; `half` is every shading language's word and MSL's spelling.
`short` says nothing of its width, and the 64-bit types follow the same pattern as `long`, `ulong` and `double` when they come.
One feature for 16-bit floats, storage and arithmetic together, is the shape WebGPU's `shader-f16` has.
16-bit ints are a second feature because WGSL has none, and one feature for both would cost WebGPU its 16-bit floats.
A device with 16-bit storage and no 16-bit arithmetic grants neither, which sg's coarse features accept for a difference of degree on old hardware.

## CHK-349

The rule is the same for any struct, since SGL's vectors are prelude structs of one-character fields and not a type the checker knows apart.
The annotation keeps it opt-in: a struct of `a` and `b` fields does not grow `e.ba` and `e.aa` by accident.
Adding swizzles to such a struct later breaks nothing, where removing them would.
`rgba` is not a second alphabet: the letters are the field names, so a port writes `.xyz` for `.rgb`, one spelling per read.

## CHK-350

A permuted position is no position, and a swizzle is the everyday way out of the strong types into plain numbers, `clip.xyz`.
So the result is the plain vector whatever the source, and `bool` vectors have swizzles too, since nothing in the rule is numeric.

## CHK-351

A swizzle found only after a failed lookup would mean whatever else is in scope, and in SGL that is not hypothetical: a dot call has the free function's candidates (CHK-247).
A module that declared `fun xy(a: float4)` would then silently retarget every `v.xy` in every file that sees it.
Found where a field is found, `v.xy` depends on the type of `v` alone.
The price is that a prelude vector cannot later grow a method named like a swizzle.

## CHK-353

FSR's permutations are read deep in shared helpers, and an option is the one shape where a helper reads one without every caller learning of it.
A compile per set of values is what dx12 pays under any mechanism, since it has no specialization constants.
slib's content-keyed cache makes the second acquire of a set free.
The targets' own specialization constants were the alternative, and they keep both sides of every branch in the text.
The footprint and the bound resources would then be the union of every variant, which is exactly what FSR's binding swap exists to avoid.
A switch that changes an image's format specializes on no target at all.
A preprocessor would break what the syntax is built on: local errors, lossless parsing, and one tree for the language server.

## CHK-355

The host is generated from the options an entry point reaches, before any value is chosen, so the set must be one for every value.
A set read off the flat tree would lose an option named only inside a branch another option removes, and the host could then never set it.
Counting what the declarations name is a little wider than what one compile reads, and an option too many costs only a key.

## CHK-356

An option is only worth having if what it turns off is gone.
It leaves the text, so the target compiler never sees it, and the footprint, so a binding the branch alone used is bound by nobody.
Leaving it to the target compiler would strip the code and still leave the footprint covering it, which is the cost FSR's permutations exist to avoid.
The guarantee holds for any constant, so a helper called with a literal flag sheds its other side too.

## CHK-357

`1u` and `0.5f` port as written, and the width defaults to 32 because that is what every target's own short suffix means.
A suffix fixes the type outright, since saying the type is the only reason to write one where CHK-253 would have converted the literal anyway.

## CHK-358

The prelude's own comment calls the plain vectors plain numbers, all of whose arithmetic is componentwise, and this is that sentence taken literally.
Every target mixes a vector with its scalar under every operator, so `uv + 0.5` and `id.xy % 2` are what ported code writes.
`vec3`, `pos3` and `hpos4` are left out because their arithmetic is the point of having them.

## CHK-362

`==` is what a `case` matches with, so it means "equal" on every type, a vector included, and gives one `bool`.
An ordering has no whole-value meaning, so componentwise is its only reading, and it is the comparison ported code writes most.
The cost falls on one spelling: a port's `all(a == b)` is `a == b`, and its `select(a == b, …)` is `select(equal(a, b), …)`.

## CHK-365

HLSL's `?:` hides two different things: control flow, where only the chosen arm runs, and data selection, where both values exist and each component picks.
`select` is the second, and the only consumer a `bool` vector has; the `if` of CHK-375 is the first.
Having both makes a port say which one it meant.

## CHK-366

Every call is inlined, so a resource parameter is substitution, exactly as a parameter of function type is (CHK-318), and no local ever holds a resource.
That is why the footprint follows the member through the call as it follows it through a builtin.

## CHK-367

A subscript is the spelling every port writes for a texel, and the one an `@atomic` image needs for its texel to have methods (CHK-373).
It is sugar over `load` and `store`, so a texel can never mean anything those calls do not.

## CHK-368

`globallycoherent` is the attribute every port of a cross-workgroup handoff already has, one to one, and a per-member fact sg can carry.
Device-scope acquire and release on atomics would be the precise tool, and HLSL has no orderings on `Interlocked*`.
So dx12 would write `globallycoherent` and fences anyway, and the memory model they need is the hardest text a language spec holds.
WGSL has neither coherence nor ordering across workgroups, so a WebGPU build of such a pass is two dispatches whatever SGL offers.

## CHK-369

A host that stages a whole C struct, as AMD's FSR host code does, needs the layout promised rather than generated.
`.hlsl` promises what is emitted today, so it costs nothing but the freedom to reorder that block.
`.cpp` is for a host struct written with no knowledge of HLSL's rows; it costs a memory form or offsets on every target, HLSL included.

## CHK-371

RDNA runs a compute shader in subgroups of 32 or 64, and AMD tunes some of FSR's passes for 64, which are correct at either size.
A preference is exactly what that annotation means, and it keeps a portable shader portable: no feature, and no device refuses it.
A shader that is correct at one size alone needs a requirement, a later and separate spelling, since turning a preference into one would refuse shaders that work today.
The name is not `@subgroup_size`, which is the stage input that reads the size the shader got.

## CHK-372

A buffer's atomic is a property of the place, not of the call, so a plain access can never race an update; an atomic image keeps that by type.
Methods on any 32-bit integer image were the alternative, and they allow a plain `store` to race an atomic update of the same texel.
An image used atomically in one pass and plainly in another is two members, or two bindings of one view.

## CHK-374

CHK-283 makes every read of workgroup memory non-uniform, since a thread may store between a barrier and the read.
So a branch on a flag one thread published cannot hold a barrier.
Proving the read uniform in the pass would be an analysis of memory, which crosses every inlined call and loop, and WGSL's own analysis would still refuse the text.
A trusted annotation would be a hang where it is wrong.
WGSL's `workgroupUniformLoad` is sound by construction, and HLSL and MSL need nothing but a barrier before the load.

## CHK-375

An `if` with an `else` is a `case` over a `bool` in all but its spelling, and it is lazy.
An arm with an effect, or one that must not be computed, runs only where it is taken.
That is the control-flow half of HLSL's `?:`; `select` is the other half (CHK-365).

## CHK-376

The whole family is designed at once so that it has one naming pass, one feature and one uniformity rule; the quad swaps a single-pass mip reduction needs are three of them.
`subgroup_` is WGSL's and Vulkan's word, and no one vendor's, where `Wave` is HLSL's and `simd` MSL's.
One feature is the shape WebGPU's own has, and splitting quad operations out would buy only devices that report a partial set.

## CHK-377

Every target defines a subgroup operation over the invocations active at the call, and they disagree on what is active after a divergent branch.
Requiring uniform control flow is sound on every target with no new analysis, and it relaxes additively, where a permissive rule could not be taken back.
A result is non-uniform for SGL's pass because uniform across a subgroup is not uniform across a workgroup.

## CHK-380

HLSL forms a compute stage's quads from its threads' ids, as it forms the quads a derivative compares: four consecutive ones of a workgroup that is one row, and 2x2 squares of one that is not.
DXC writes them for vulkan in the same derivative groups, which is why sg's vulkan grants `subgroups` only with linear compute derivatives.
WGSL and MSL form quads of four consecutive invocations of a subgroup, which agree with the row and with no square.
One row whose length is a multiple of 4 is the one shape every target forms alike, and it is the shape a single-pass reduction already takes.
