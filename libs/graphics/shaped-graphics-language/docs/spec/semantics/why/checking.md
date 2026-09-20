# Why: checking

*Tracer: deliberately thin.*

The reasons behind the rules of [checking.md](../checking.md).
Nothing here is normative.

The pass as a whole follows the [compilation model](../../incubator/compilation-model.md): name resolution, type checking and evaluation cannot be separated.
Overloading is by type, so even an unqualified name needs types to resolve, and a type expression may need evaluation.
So there is no resolver that runs first and no tree of resolved names: there is one demand-driven pass.

## CHK-2

The prelude is a file on disk, and placing it in front needs no multi-file compilation.
One unnamed module is the smallest thing that holds two files, and it is what a module will be once several files declare one.
Concatenating the two texts would have been less code, and every span of the program's file would then be off by the length of the prelude.

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

Nothing demands a body yet.
A body is needed by a caller for two reasons only: to infer a return type and to inline the call, and the tracer does neither.
Checking bodies after the signatures also keeps an overload set usable from inside one of its own members.
With a demanded body that would be a cycle, since resolving the name asks for every candidate.
This moves once the inliner exists: recursion is then found where a function in compilation is inlined into itself.

## CHK-30

The alternative to a name is an id in the attribute, `@builtin(normalize_vec3)`, and it says everything twice.
A name keeps the prelude readable as the documentation of the builtin functions, which is its second job.
The parameter types then pick among the builtins of one name, as they do for any overload set.

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
A second local of one name inside a function is a plain `duplicate-declaration`, since a function body is one scope today.

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
