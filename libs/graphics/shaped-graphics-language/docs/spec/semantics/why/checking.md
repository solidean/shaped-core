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

## CHK-54

Whether a local may shadow a module-level name is open in [scopes](../../incubator/scopes.md).
Both answers are easy to give later and neither is easy to take back, so the tracer gives none.
A second local of one name in one block is a plain `duplicate-declaration`.

## CHK-110

Whether an inner block may shadow is open in [scopes](../../incubator/scopes.md), like the module-level case of CHK-54.
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

The language's model is that a body is checked where it is inlined, since a generic has no types before that.
No function is generic yet, so a body means the same at every call, and checking it once gives the same answers.
Checking it once is also what reports an error in a function nobody calls, and what reports it one time where three calls would report it three times.
Once generics arrive, a generic body is checked per inline and every other body stays here.

## CHK-131

A binding is a global of the target, so a callee reads it without being handed it.
The list on the caller is what keeps that visible: an entry point's list names everything its shader touches, and the pipeline layout follows from that list alone.
The check is per call and needs no walk: each caller answers for its callees, so the chain closes by induction.

## CHK-72

Without implicit conversions a match is exact, so two matching candidates have the same parameter types.
That could be reported where the second one is declared.
It is reported at the call because a declaration's parameter types are only known once it is compiled, which a call demands and a declaration does not.
Once conversions exist, ambiguity at the call is the rule that is needed anyway.

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
Until SGL has generics, an overload family cannot be written once in SGL, so the registry's C++ loops write the families out and `core.sgl` holds next to nothing.

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
