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

## CHK-259

A `require` is permission, and what an entry point needs is judged from its use (CHK-263).
So a file-level `require` costs nothing: a vertex stage in a ray-tracing file still runs on a device that cannot trace.
The alternative, where the file's `require` is every entry point's floor, would make the cheapest place to write it the most expensive one to have written.

## CHK-261

A binding is a layout the host binds whole, so what one of its members needs is needed wherever the binding is listed, read or not.
Its own `require` is therefore a requirement of the binding and not only a grant to its members: a library that ships a binding says, in the binding, what a device must have to take it.

## CHK-263

The floor is defined by what the language counts, never by what an optimizer happens to remove.
A floor that followed dead-code elimination would change with a compiler version or an unrelated refactor, and the host would find out on a device that lacks the feature.
So every use the entry point reaches counts, including one behind a condition that is always false.
A compile-time branch on a feature, `if feature raytracing:`, is the form that may leave a use out, and it is not built.

## CHK-265

A binding's `require` is how a library says what a device must have to take the binding, and a member need not be what uses it.
A binding that carries an acceleration structure later, or that a caller's shader reads through a feature, states the need before anything in SGL can show it.
So only a body's `require` can be unused: it says nothing about any binding, and it is the one place an unneeded line is certainly a mistake.
