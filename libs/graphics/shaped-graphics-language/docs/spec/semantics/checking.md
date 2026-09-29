# Checking

*Tracer: deliberately thin.*

The check pass reads the ASTs of one module and resolves names, checks types and builds the flat tree of every entry point.
It is one pass, not three, and it carries what three samples need.
They are [cube.sgl](../../../tests/samples/cube.sgl), [helpers.sgl](../../../tests/samples/helpers.sgl) and [matrices.sgl](../../../tests/samples/matrices.sgl).
Back to the [semantics](_index.md); the reasons are in [why/checking.md](why/checking.md).

## The pass

* **CHK-1** The check pass reads the [ASTs](../syntax/ast.md) of the files of one module, and it changes none of them.
* **CHK-2** The module is unnamed: the files of the prelude stand in front, then the program's file, and a file is named by its position in that order ([why](why/checking.md#chk-2)).
* **CHK-3** A `module` line is accepted and names nothing.
* **CHK-4** The files stay separate, so a span points into the source of its own file, and a diagnostic names its file.
* **CHK-5** The check pass is total: any ASTs give a checked module and a list of diagnostics, `invalid` nodes and earlier diagnostics included.
* **CHK-6** What did not check has the **error type**.
* **CHK-7** The error type never causes a second diagnostic: whatever meets it is silent ([why](why/checking.md#chk-7)).
* **CHK-8** A construct of the language that the pass does not carry is the normal error `unsupported-yet`, and its detail names the construct ([why](why/checking.md#chk-8)).
* **CHK-9** The pass gives a construct it does not carry no meaning: its type is the error type.

## Symbols

* **CHK-10** A **symbol** is a `fun`, a `struct`, an `enum` or a `binding` declared at the top level of a file, named by its file and its declaration.
* **CHK-11** The module scope is unordered: a symbol may be used above its declaration.
* **CHK-12** A name is declared once in one scope, unless every declaration of it is a function, or one is a `struct` and every other a function (CHK-240).
  A later declaration is the normal error `duplicate-declaration`.
* **CHK-13** Several functions of one name are an **overload set**.
* **CHK-188** The module scope is two: the files of the prelude share the outer one, and the program's file has the inner one ([why](why/checking.md#chk-188)).
  A declaration of the program's file **shadows** what the prelude declares of its name, so a `struct vec3` there is no duplicate, and it shadows that struct's constructors with it.
* **CHK-189** Where both scopes declare nothing but functions of one name, the functions of both are one overload set.
  So are they where the prelude declares a struct and the program nothing but functions of its name: a program's `fun float4(v: float)` joins `float4`'s constructors.
* **CHK-192** Where a call matches functions of both scopes, those of the program's file are its only matching candidates ([why](why/checking.md#chk-192)).
  It is applied before CHK-254 ranks them, so two matches in one scope are still ranked.
* **CHK-190** A lookup from a prelude file sees the prelude's scope alone, and never a name of the program's file.
* **CHK-191** What the check pass needs of the prelude by name is always the prelude's, whatever the program's file shadows.
  That is the type of a literal, of a condition and of a `for`, and `raster_pipeline_description`.
* **CHK-14** A `type` alias is `unsupported-yet`, and it still owns its name, so a use of it is silent; a `const` is carried by CHK-219.
  An `enum` is a symbol of its own, by CHK-142.
* **CHK-15** `use` and `notation` are `unsupported-yet`.
* **CHK-16** A symbol is in one of four states: untouched, in compilation, checked, or failed.
* **CHK-17** Compilation is on demand: needing a symbol that is untouched compiles it first ([why](why/checking.md#chk-17)).
* **CHK-18** Needing a symbol that is in compilation is the normal error `dependency-cycle`, its detail names the loop, and what needed it gets the error type.
* **CHK-19** A failed symbol gives whatever needs it the error type, without a diagnostic.
* **CHK-20** Compiling a function means its signature; a body is checked after every signature is known, except by CHK-135 ([why](why/checking.md#chk-20)).

Two structs that hold each other report `dependency-cycle` with the detail `a -> b -> a`.

```sgl sketch
struct a:
    x: b

struct b:
    y: a
```

## Types

* **CHK-21** A type is canonical: two expressions name the same type exactly when they resolve to the same type, and nothing converts implicitly but a literal, by CHK-81 and CHK-253.
* **CHK-22** Each `struct` declaration is one type, whatever its fields.
* **CHK-23** A type position holds a name that resolves to a `struct` or an `enum`, or `void` (CHK-215); every other expression there is `unsupported-yet`.
  The exceptions are named where they are specified: a resource type (CHK-198), an array (CHK-285), a function type (CHK-317), a type parameter (CHK-338) and an instance of a generic struct (CHK-339).
* **CHK-24** A name in a type position that is not declared is the normal error `unknown-name`, and one that stands for a function or a binding is `wrong-kind-of-name`.
* **CHK-25** A format is not a type: nothing such as `rgba8` exists.
* **CHK-26** A field, a binding member and a parameter have a type; one without is the normal error `missing-type`.
* **CHK-27** A `mut` member is `unsupported-yet`; defaults, properties and methods are [members](#members-and-constructors).
* **CHK-28** Two fields of one struct, two members of one binding and two parameters of one function differ in name, or the later one is `duplicate-declaration`.

## Members and constructors

* **CHK-233** A struct and an enum each have a **type scope**: the properties and functions declared in its block, and its extensions (CHK-237).
  A field and a case are members of the type, and no function.
* **CHK-234** A `fun` in a type's block whose first parameter is `self` is a **method**, and `self` is a parameter of that type; `mut self` is CHK-48.
* **CHK-235** A `fun` in a type's block without `self` is a **static**, a function of the type scope like a method.
* **CHK-236** A property `name => value` is a function of the type scope whose one parameter is `self`, and whose result is the type of `value` by CHK-122.
  An extension property with `-> T` returns `T`, and its value is expected to be of `T` by CHK-82.
  A property's `=>:` block yields on every path, or it is `missing-return`, as CHK-125 has a function return; an extension property without a body is `expected-body`.
  A property is read-only: an assignment to it, or to a member of it, is `not-assignable`.
* **CHK-237** An extension `fun T.name` (AST-142) declares a function of the type scope of `T` from outside it: a method with `self`, a static without, and a property without parameters (AST-143).
  It is visible where a name of its file is visible, so one of the program's file is none of the prelude's (CHK-190).
  `T` names a struct or an enum, or it is `wrong-kind-of-name`.
  An extension inside a type's block is `unsupported-yet`; it is meant to extend `T` where that block alone sees it.
* **CHK-238** A type scope holds one kind of thing per name: a field, a case, a property and functions of one name are the normal error `member-name-clash`, at the later one.
* **CHK-239** A `struct` with a block has a **synthesized constructor**: a function of the struct's name, declared where the struct is, which returns the struct.
  Its parameters are the fields in field order, each with the field's type, default and named-only mark.
* **CHK-240** Functions of a struct's name may be declared wherever a `fun` may stand, and they are one overload set with its synthesized constructor where they are visible.
* **CHK-241** Two functions of one overload set in one scope whose parameters agree in type, name and named-only mark, in order, are `duplicate-declaration`, at the later one.
  The synthesized constructor is one of them.
  Across the prelude and the program's file, CHK-192 hides the prelude's instead.
* **CHK-242** A parameter may carry a default, `= value`, which makes it optional; a field's default is the default of its constructor parameter.
  So a field's default reads the fields before it bare, `inner: float = radius * 0.5`: they are the constructor's parameters, and no `self` exists yet.
* **CHK-243** A default may read the parameters before it and what its function's scope sees, and never what the call sees ([why](why/checking.md#chk-243)).
  A method's `self` is one of them, so a default reads `self.scale`.
  It is checked once, where it is declared, and it is of its parameter's type or `type-mismatch`.
* **CHK-244** A named-only parameter (AST-144) binds by name alone, and a named-only field makes its constructor's parameter named-only.
* **CHK-245** `self` in the body of a method or a property is its receiver, and anywhere else it is `unknown-name`.

```sgl
struct falloff:
    radius: float
    inner: float = radius * 0.5
    .sharpness: float = 1.0
    width => self.radius - self.inner
    fun at(self, d: float) -> float => saturate((self.radius - d) / self.width)
    fun unit() -> falloff => falloff(1.0)

fun falloff.doubled => falloff(self.radius * 2.0, sharpness = self.sharpness)
```

## Void

* **CHK-215** `void` is a reserved name ([AST-137](../syntax/ast.md#atoms)): in a type position it is the type `void`, and anywhere else it is that type's one value.
* **CHK-216** `void` is a type like any other: a local, a parameter, a field and a result may be of it, and `let`, `return` and `print` take its value.
* **CHK-217** `==` and `!=` over two `void` values are the language's own and give `true` and `false`: both operands run, and the answer is known before either does.
* **CHK-214** A binding member of type `void` is `type-mismatch`: a member is a slot of the group's layout, and a void one fills none.

```sgl
fun note(x: float) -> void:
    print x

fun f(x: float) -> float:
    let done = note x
    if done == void => return x
    return 0.0
```

## Consts

* **CHK-219** A `const` at file scope stands for its value wherever it is named: an `int` or `float` literal, `-` in front of one, an enum case, or another `const`.
  Any other value is `unsupported-yet`, and a written type the value does not have is `type-mismatch`.
* **CHK-221** A `const` whose value is an enum case names that case as a `case` pattern, so it counts for exhaustiveness as `e.case` does (CHK-159).
* **CHK-222** `true` and `false` are `@shadowable(false)` consts of `core.sgl`, whose values are the cases of `bool` (CHK-218).

```sgl
const steps = 4
const scale = -0.5

fun sign(b: bool) -> float:
    return case b:
        true => 1.0
        false => scale
```

## Enums

* **CHK-142** An `enum` declaration is a symbol and one type, whose kind is `enumeration`; two declarations are two types, as CHK-22 says of a `struct`.
* **CHK-143** A member of an `enum` block that is a case gives the type one **case**, named by that member, in the order written.
* **CHK-144** A case has an `int` value: the one it writes, or one more than the case before it, and 0 for the first.
* **CHK-145** Two cases of one enum may hold one value, so `all = 7` beside `red = 1` is a legal alias; two that hold one name is `duplicate-declaration` by CHK-28.
* **CHK-146** The written value of a case is an `int` literal, by CHK-61; any other expression there is `unsupported-yet`.
* **CHK-147** `enum.case` is an expression of the enum's type, and a name the enum has no case of is `unknown-member`.
* **CHK-148** An enum's own name is no value, by CHK-63.
* **CHK-149** `==` and `!=` over two values of one enum are the language's own, as `and` is by CHK-116: no function declares them, and each gives a `bool`.
* **CHK-150** An enum converts to no type and no type converts to it, `int` included, and it has no other operator ([why](why/checking.md#chk-150)).
* **CHK-151** A nested declaration in an `enum` block is `unsupported-yet`; its properties and methods are those of a struct (CHK-233).
* **CHK-218** `bool` is a `@builtin enum` of the prelude with the cases `false` and `true`, in that order, so `bool.true` is a value like `light_kind.sun`.
  Its record makes it the targets' bool: CHK-149 and CHK-150 do not hold for it, since its `==` is the prelude's and `and`, `or` and `not` take it (CHK-116).
* **CHK-321** `@bitflags` on a `@builtin enum` makes its cases bits, which combine: its registry gives `|`, `&` and `has`, and a value may be several cases or none.
  `ray_flags` is one, with DXR's values.
  `@bitflags` on an enum of the program is `unsupported-yet` ([enum futures](../incubator/enum-futures.md)).

```sgl
enum light_kind:
    point
    spot
    sun
```

## Tests

* **CHK-224** A `test` is checked as a function of no parameter that returns `void`, on its own, wherever it stands: at file scope, in a struct or an enum, or in a function body.
  Its bindings are the ones it lists (CHK-333).
  One in a function body is checked after that body, and it never runs where it stands, so no jump in front of it makes it unreachable.
  Every `test` the source spells is run or fails, and none is ever left out.
  One whose surroundings are never checked, a function whose signature failed or a test inside a test, is checked as one at file scope.
* **CHK-225** In a test, and not in a function nested in one, an expression statement of type `bool` is a **check**: it must be true when it runs ([why](why/checking.md#chk-225)).
  Any other expression statement there that is no paren or juxtaposition call, and whose type is not `void`, is the warning `no-effect`, which the AST pass leaves to this one (AST-140).
* **CHK-226** The last code line of a test is a check, or it is `test-must-end-in-check` ([why](why/checking.md#chk-226)).
  The last code line is the last statement of its body, through the last branch of an `if`, the body of a loop and the last arm of a `case`.
  A test whose asserts are what it checks ends in `true // why`.
  A test that expects `.fail`, `.assert` or `.discard` is exempt, since it cannot pass without its run ending so, and so is a test with no statement, which the AST pass reported.
* **CHK-227** `assert condition` takes a `bool`, in a test or anywhere else; a message is `unsupported-yet`.
  A condition that writes a buffer, prints, or calls a builtin with an effect is `unsupported-yet`, since no target writes an `assert` (LEGAL-53) and its effect would happen on the interpreter alone.
* **CHK-228** A test reads nothing of the function it stands in: a parameter, a local or a binding member of it is `test-captures-runtime-value`, since the test runs on its own.
  Those names are still visible, so they hide what the module has of the name; a `const` is no value of a run and may be read.
  A binding member the test lists is its own and no capture, whatever the function lists.
  A callee that needs a binding the test does not list is `binding-not-listed` by CHK-131, with a note that listing it gives it.
* **CHK-333** `test {a, b}:` lists the bindings the test reads, as a function's `{...}` does, and the driver that runs the test gives their values (EVAL-94).
  A binding it lists holds values, buffers and acceleration structures; a texture, an image or a sampler in one is `unsupported-yet`.
  A `@workgroup` binding needs no listing (CHK-295).
* **CHK-229** In the flat tree a check or an `assert` is a `check` statement, whose body leaves every node of the condition in a `var` of its own.
  A node is an `and`, an `or`, a `not`, a comparison, a comparison chain, or a leaf any other expression is; it runs in the order and under the conditions the condition itself would run it.
* **CHK-230** A test whose body checked clean, and whose every callee inlines whole, has a flat tree of its own, of no stage and without a parameter.
  A test that expects a diagnostic has none, and neither has one in whose text the parser or the AST pass found an error.
* **CHK-231** `@expect` on a test names what it does, one argument each: `.fail`, `.assert`, `.discard`, `error = "kind"` or `warning = "kind"`.
  In a kind, `*` stands for any run of characters and `?` for one.
  An empty kind is `invalid-attribute-arguments`.
  Any other argument is `invalid-attribute-arguments`.
* **CHK-278** A test that expects `.discard` passes where its run ends at a `discard`, and like one that expects `.fail` or `.assert` it need not end in a check.
* **CHK-298** A test whose run reaches a builtin that takes derivatives is `stage-not-allowed` at that call: a run is one invocation, and has no quad to take one across.
  A barrier waits for nobody in it, and an atomic updates the memory of its own run (EVAL-92, EVAL-93).
* **CHK-232** In a test with an `error` or a `warning` expectation, every diagnostic of any phase inside it is the test's, and is reported nowhere.
  Inside is from its keyword to the end of its body.
  One expected diagnostic usually brings others with it, and the test exists to show that the one it names is reported.
  An expectation none of them meets is `unmet-expectation`, and a test that expects a diagnostic is judged by that alone and never run.
  A test that expects `.fail` passes where a check or an `assert` of its run is false, and one that expects `.assert` where its run stops at a false `assert`.
  Several of them on one test must all hold of its one run.

```sgl sketch
fun shade(k: float) -> float:
    @expect(error = "test-captures-runtime-value")
    test k > 0.0
    return k

@expect(.fail)
test 1 > 2
```

```sgl
fun square(x: float) -> float => x * x

test square 3.0 == 9.0

fun shade(k: float) -> float:
    test:
        let x = square 2.0
        x == 4.0
    return k
```

## Builtins and the prelude

* **CHK-29** The prelude is SGL source, and each of its files is checked like the program's file; CHK-138 says which files it has.
* **CHK-30** A declaration that carries `@builtin` stands for one record of the compiler's **builtin registry**.
  The key of a `struct` or an `enum` is its **name**, and the key of a `fun` is its name together with its parameter types, so every overload is a record of its own ([why](why/checking.md#chk-30)).
* **CHK-31** A `@builtin` declaration no record has the key of is the normal error `unknown-builtin`.
  That is a name the registry does not hold, a name it holds as the other kind of declaration, or parameter types no overload of the name takes.
* **CHK-32** A `@builtin fun` has no body, and every other `fun` has one, or it is the normal error `expected-body`.
* **CHK-33** A `struct` without a block is **opaque**: it has no fields and no constructor.
* **CHK-34** An opaque struct carries `@builtin`, or it is the normal error `opaque-struct-needs-builtin`.
* **CHK-35** A builtin struct with a block has real fields, and they are read like the fields of any struct.
* **CHK-36** `@operator("*")` on a function names the operator it stands for, as one quoted literal; anything else is the normal error `invalid-attribute-arguments`.
* **CHK-37** An `@operator` function is found through its operator alone: its own name is in no scope ([why](why/checking.md#chk-37)).
* **CHK-38** The pass knows the attributes of the table below, each on the node kind the table names.
* **CHK-39** Any other attribute, anywhere, is `unsupported-yet`, and arguments on a known attribute that takes none are `invalid-attribute-arguments`.
  An attribute that names a pipeline setting is known where CHK-181 says, and takes its value.
* **CHK-106** `@pure` on a function says that a call of it has no [effect](evaluation.md#effects); a `@builtin` without it is assumed to have one ([why](why/checking.md#chk-106)).

| on | the known attributes |
|---|---|
| a function | `@builtin`, `@pure`, `@operator`, `@vertex`, `@pixel`, `@compute`, `@geometry`, `@tessellation_control`, `@tessellation_evaluation`, `@stages`, `@shadowable`, `@expect`, `@internal` |
| a function, as a ray-tracing stage | `@raygen`, `@miss`, `@closest_hit`, `@any_hit`, `@intersection`, `@callable` |
| a struct | `@builtin`, `@vertex`, `@pixel`, `@shadowable`, `@no_padding`, `@internal` |
| an enum | `@builtin`, `@shadowable`, `@bitflags`, `@internal` |
| a const | `@shadowable` |
| a test | `@expect` |
| a binding | `@inline`, `@workgroup`, `@shadowable`, `@no_padding` |
| a binding member | `@unfilterable`, `@non_filtering`, `@sampler` |
| a struct field | `@position`, `@per_instance`, `@stream`, `@interpolate`, `@format` on a `@vertex struct`, `@depth` and `@sample_mask` on a `@pixel struct`, `@edge_factors` and `@inside_factors` on any other |
| a parameter | the stage inputs of CHK-271 |
| a pipeline | `@raster`, `@compute`, `@raytracing` |

* **CHK-323** `@internal` on a declaration of the prelude makes it the prelude's alone: no lookup from the program's file finds it, and the prelude's files still do.
  On a declaration of the program's file it hides nothing.
* **CHK-324** A function of the prelude may take a resource, which a function of the program may not (CHK-206).
  Inlining substitutes it: the argument stands wherever the parameter is named, since no target holds a resource in a local.

```sgl
@builtin struct float

@builtin struct vec3:
    x: float
    y: float
    z: float

@builtin @pure fun dot(a: vec3, b: vec3) -> float
@builtin @pure @operator("+") fun add(a: float, b: float) -> float
```

## Bindings

* **CHK-40** A `binding` with a block is a symbol whose members have a name and a type; the composition form is `unsupported-yet`.
* **CHK-41** `@inline` on a binding is recorded on the checked binding, since an emitter needs it.
* **CHK-42** Each entry of a function's binding list is the bare name of a binding; any other entry is `unsupported-yet`.
* **CHK-43** A binding list is checked and never passed ([why](why/checking.md#chk-43)).
* **CHK-44** `binding.member` is an expression of the member's type, inside a function whose binding list names that binding.
* **CHK-45** In any other function it is the normal error `binding-not-listed`.
* **CHK-46** A binding is no value: its bare name in an expression is `unsupported-yet`.
* **CHK-171** A buffer member is known to the host by its path, `binding.member`, and a group's constant block by the binding's name ([why](why/checking.md#chk-171)).
* **CHK-198** A texture, an image and a sampler are types of a binding member, as a buffer is, and none is a value: [bindings.md](../bindings.md) is the model.
  Two mentions of one such type are one type, and a member names each of them as its spelling does: `texture_2d[float4]`, `out image_2d[.rgba8_unorm]`.
* **CHK-199** A texture's argument is `float`, `int` or `uint`, one to four wide, and a texture without its argument is `wrong-kind-of-name`.
  Another type as the argument is `wrong-kind-of-name` too, and a name that is no type is `unknown-name` by CHK-24, so `texture_2d[rgba8]` is the latter.
* **CHK-200** An image's argument is exactly one enum case naming one of sg's image formats, `.rgba8_unorm`.
  It is the one value type argument SGL reads, until value type arguments exist in general.
* **CHK-201** A form some backend lacks is the normal error `needs-feature` on every target alike, unless a `require` grants its feature ([Features](#features)):
  `texture_2d_ms_array`, an image outside the portable image formats, and a `mut` image outside the three `r32` formats.
* **CHK-202** `@unfilterable` stands on a texture member of floats, and on any other binding member is `wrong-kind-of-name`.
  Elsewhere it is an attribute the pass does not know, by CHK-39.
* **CHK-203** `@non_filtering` stands on a `sampler` member, and on any other binding member is `wrong-kind-of-name`.
  Elsewhere it is an attribute the pass does not know, by CHK-39.
* **CHK-204** A `sampler name:` block in a binding is a member whose type is `comparison_sampler` where it sets `compare`, and `sampler` otherwise.
  Its settings are `sg::sampler`'s fields, each an enum case or a number; an unknown setting or value is `invalid-attribute-arguments`.
  The settings apply in source order, and a later one overrides what an earlier one set, `filter` over `mip_filter` included.
  An attribute on the block is judged as on any other binding member.
* **CHK-205** A static sampler in an `@inline` binding is `wrong-kind-of-name`, since such a binding holds constants only.
* **CHK-206** A `@builtin` function alone may take a texture, an image or a sampler, and a function of the prelude by CHK-324.
  For any other function each is `unsupported-yet`, as it is anywhere a value stands.
* **CHK-207** A builtin's image parameter names the texel it loads or stores instead of a format, `out image_2d[float4]`, and is a pattern:
  it takes every image of that shape whose format's texel is that type, and which the shader may read where the pattern reads, or write where it writes.
* **CHK-194** A builtin's bare `texture_2d` or `image_2d` parameter is a pattern too, which takes every texture, or every image, of that shape, whatever it holds and however it is read.
  It is for what depends on neither, such as a size.
* **CHK-210** A call that hands a builtin an `@unfilterable` texture member and a sampler member that filters is `type-mismatch`, and its detail names both.
  A sampler a texture's `@sampler` supplies counts as handed (CHK-279).
  A `sampler` member filters unless it is `@non_filtering`, and a static sampler filters unless every filter is `.nearest`.
* **CHK-211** `max_anisotropy` is an `int` literal from 1 to 16, and anything else is `invalid-attribute-arguments`.
* **CHK-212** A static sampler whose `max_anisotropy` is above 1 has every filter `.linear` once its settings are applied, or it is `invalid-attribute-arguments`.
* **CHK-279** `@sampler(name)` stands on a texture member and names a sampler member of the same binding, static or dynamic, or a file-scope sampler (CHK-314).
  A member of the binding hides a file-scope name.
  On any other member it is `wrong-kind-of-name`; a name neither the binding nor the file has is `unknown-member`, and one that is no sampler `wrong-kind-of-name`.
  A texture method called without its sampler takes the texture's `@sampler`, which is `missing-sampler` where the texture has none,
  and `type-mismatch` where it is not the kind the call takes: a `comparison_sampler` for a comparison, and a `sampler` otherwise.
* **CHK-280** A texture method's `offset` and `component` are constants: a literal, an enum case, a `const`, or a construction of those.
  An offset's literals are from -8 to 7, and a comparison's `level` is the literal `0.0`; anything else is `invalid-constant-argument`.
* **CHK-281** A call that samples a depth texture through a sampler that filters is `type-mismatch`, as CHK-210 is for an `@unfilterable` texture.
  A comparison takes a `comparison_sampler`, which is none of the samplers this counts.
* **CHK-314** A `sampler name:` at file scope is a symbol whose type is `comparison_sampler` where it sets `compare`, and `sampler` otherwise.
  Its settings and its attributes are judged as CHK-204 judges a binding's static sampler.
  It is used by its name, handed to a builtin as CHK-206 says, or through a texture's `@sampler` (CHK-279).
  Either way it filters as a static sampler does for CHK-210 and CHK-281.
  Its name anywhere else is `unsupported-yet`, and so is its name in a test, which samples no texture.

```sgl
@inline binding constants:
    view_projection: mat4

@vertex fun main_vs(v: cube_vertex){constants} -> pixel_input:
    return {
        position = constants.view_projection * v.position
        normal = v.normal
        color = v.color
    }
```

## Functions

* **CHK-47** A function has typed parameters, and its return type stands behind `->`; one without returns `void`, by CHK-121.
* **CHK-48** A function with `mut self` is `unsupported-yet`, and it fails as a whole; type parameters are [CHK-338](#generics).
* **CHK-315** A parameter `p: mut T` over a value type is a **mut parameter**: the caller's place, which the body may assign like a `let mut` local ([why](why/checking.md#chk-315)).
  Over a resource or a stream, `mut` is its access instead (AST-128).
  An entry point takes no mut parameter but a ray-tracing stage's payload (CHK-328), and `mut p: T` is `unexpected-keyword`, since a parameter's `mut` is written on its type.
* **CHK-49** A function whose signature holds the error type is failed.
* **CHK-50** A function body is an ordered scope: a parameter is visible from the start, and a local from the statement after its `let`.
* **CHK-51** `let name = value` introduces an immutable local of the type of `value`.
* **CHK-52** `let name : type = value` needs `value` to be of `type`, or it is the normal error `type-mismatch`.
* **CHK-53** A local **shadows** every earlier local and parameter of its name, in its own block or an enclosing one, from the statement after its `let`, as in Rust ([why](why/checking.md#chk-53)).
  The value it is given still sees the one it hides, and its type may differ.
* **CHK-54** A local or a parameter may have the name of a module-level symbol, and shadows it as it shadows a local ([why](why/checking.md#chk-54)).
  Types and values share one namespace, so behind it the name is the local wherever it stands: a call of it is CHK-78, and a type position holding it is the normal error `wrong-kind-of-name`.
  A parameter's type is resolved before any parameter is in scope, so `light: light` is fine.
* **CHK-220** A symbol that carries `@shadowable(false)` is hidden by nothing, and it stays what its name means.
  A declaration of the program, a local, a parameter or a `for` variable of its name is `shadows-unshadowable`.
  Its one argument is `false` or `true`, on any declaration it stands on, and anything else is `invalid-attribute-arguments`.
* **CHK-266** Every type of `builtins.sgl` is `@shadowable(false)`, and the types of `core.sgl` are not.
  A program's own `int` would be a second type that reads like the first, while a literal stays of the prelude's, so `level: int = 0` would read `expected int, got int`.
* **CHK-55** A pattern in a `let` and a `let` without a value are `unsupported-yet`; `let mut` is CHK-111.
* **CHK-56** `return value` needs `value` to be of the function's return type, or it is `type-mismatch`.
* **CHK-57** An arrow body `=> value` is `return value`.
* **CHK-58** A block body that returns a value does so on every path, by CHK-123 to CHK-125.
* **CHK-59** A declaration inside a function is `unsupported-yet`, a `test` excepted (CHK-224).
  `assert` is CHK-227, a call is a statement by CHK-137, and every other statement is [control flow](#control-flow).

## Expressions

* **CHK-60** A number literal with a DOT or an exponent, in decimal and without a suffix, is a **float literal**, whose **default type** is the prelude's `float`; a sign directly on it is part of it.
  At its default type as anywhere else, it is one `float` holds by CHK-253, or `literal-not-representable`.
* **CHK-61** A decimal literal of digits alone is an **integer literal**, whose default type is the prelude's `int`, and a sign directly on it is part of it.
  A number literal is of its default type wherever no other type is asked of it by CHK-253.
  An integer literal is held in 64 bits, and one beyond them is `unsupported-yet`, as is a literal with a suffix or a `p` exponent.
  One the type it ends up with does not hold, its default type included, is `literal-not-representable`: `let u: uint = 3000000000` is legal and `let i = 3000000000` is not.
* **CHK-269** A literal of hexadecimal digits behind `0x`, or of binary ones behind `0b`, is an integer literal like a decimal one ([why](why/checking.md#chk-269)).
  Its value is the number it spells: `0xffff'ffff` is 4294967295, which a `uint` holds and an `int` does not, and `-0x8000'0000` is the most negative `int`.
* **CHK-62** A name resolves to a local or a parameter first, and to a symbol of the module after that; one that resolves to nothing is `unknown-name`.
  A body reads the members of its receiver through `self` alone: a bare `radius` in a method is no field of `self` ([why](why/checking.md#chk-62)).
* **CHK-63** A name that stands for a struct, a function or a binding is no value by itself: it is `unsupported-yet`.
* **CHK-64** `value.name` is the field `name` of the struct type of `value` where it has one, and a call by CHK-249 otherwise.
  A name with neither a field nor a candidate is the normal error `unknown-member`.
* **CHK-65** `(x)` is `x`.
* **CHK-152** `.name` is the case `name` of the enum the context expects, which today is the scrutinee of a `case` and the field a pipeline setting assigns (CHK-178).
  A leading dot where no type is expected is `unsupported-yet`, and one whose expected type is no enum, or has no such case, is `unknown-member`.
* **CHK-66** Every expression kind not named in this section is `unsupported-yet`.

## Calls and overloads

* **CHK-67** A paren call `f(x)` and a juxtaposition call `f x` are the same call.
* **CHK-68** An infix or a prefix operator is a call of the `@operator` functions of its spelling, with its operands as arguments.
* **CHK-69** Operators, functions, methods, properties and constructors are one mechanism: each call resolves by CHK-70 to CHK-73.
* **CHK-247** The **candidates** of a call of `foo` are the functions named `foo` visible at the call, and those of the type scope of its first argument's type.
  They also include the functions named `foo` visible where that type is declared.
  A dot call `a.foo(…)` has `a` as its first argument, so it and the free call `foo(a, …)` have the same candidates ([why](why/checking.md#chk-247)).
  A free call has them whatever else `foo` names at the call, a struct or a const included, and is `wrong-kind-of-name` only where it has none.
* **CHK-248** `T.foo(…)`, where `T` names a struct or an enum, has the functions of the type scope of `T` as its candidates, and `T` is no argument.
* **CHK-249** `a.foo` without an argument list is the field `foo` where the type of `a` has one, and a call of `foo` with `a` as its one argument otherwise.
  A field takes part in no call: `a.foo(…)` has the candidates of CHK-247 whatever the fields of `a` are.
* **CHK-250** A candidate **binds** a call's arguments to its parameters: a positional argument fills the parameter at its position, and a named argument the parameter of its name.
  The receiver of a dot call is position 0.
* **CHK-251** A positional argument after a named one binds only where every argument before it stands at its own parameter's position ([why](why/checking.md#chk-251)).
* **CHK-252** A candidate does not bind where an argument names no parameter, a parameter is filled twice, a positional argument reaches a named-only parameter or lies past the last.
  It does not bind either where a parameter without a default is left unfilled.
  A parameter left unfilled takes its default.
* **CHK-317** A **function type** `(A, B) -> R` is the type of a parameter and of nothing else ([why](why/checking.md#chk-317)).
  A local, a field, a member, a result or an element of one is `wrong-kind-of-name`.
  Two function types of the same parameter types and result are one type.
* **CHK-318** A parameter of function type takes a function's name, an arrow lambda `x => value`, or a parameter of the same function type handed on.
  A name takes the one function of its name whose parameters and result are the type's exactly, none of them `mut`; a builtin or an entry point is none.
  A lambda takes the type's parameter types, a parameter type it writes must be the same, and its value must be the type's result.
  It is checked where it stands, and it sees every name visible there; any other lambda is `unsupported-yet`.
  A function or a lambda meets no parameter of any other type, and its chain has length 0.
* **CHK-319** A call of a parameter of function type is a call of the function it was handed, inlined where the parameter is called.
  A lambda is written out there with the names it saw where it was written, so a function value is never a value of any target.
* **CHK-316** `mut x` in a call hands over a place, and it fills a mut parameter alone ([why](why/checking.md#chk-316)).
  A mut parameter takes an argument marked `mut` and nothing else, and a marked argument binds to nothing else.
  The argument is a place by the rules of an assignment's left side, or `not-assignable`, and its type is the parameter's exactly, so its chain has length 0.
  Its indices are evaluated once, where the call binds it, and the body reads and writes that one place wherever it names the parameter.
* **CHK-70** A candidate **matches** when it binds, and each argument converts to its parameter's type by a **conversion chain**.
  An argument of the parameter's type does so by a chain of length 0, and so does one a pattern parameter takes (CHK-194, CHK-207); a literal converts by CHK-81 or CHK-253.
* **CHK-253** A number literal converts to a numeric type that holds it: by a chain of length 0 to its default type, and of length 1 to any other ([why](why/checking.md#chk-253)).
  An integer type takes an integer literal whose value it holds exactly.
  A float type takes a float literal rounded to nearest within its range, and an integer literal it holds exactly.
  A float literal converts to no integer type.
  A literal that meets one expected type which does not hold it is the normal error `literal-not-representable`, and in a call its candidate does not match.
* **CHK-254** Of two matching candidates, one is **better** when its chain is no longer for any argument and shorter for at least one ([why](why/checking.md#chk-254)).
  The best is better than every other.
  A default costs nothing, so `f(x)` beside `f(x, y = 1)` leaves `f(v)` without a best.
* **CHK-255** Among matching candidates whose chains are of equal length at every argument, a function of a type scope is better than one found by name at the call.
* **CHK-257** An operator whose operands are all integer literals, and whose best candidate converts one of them, is the normal error `literal-needs-type` ([why](why/checking.md#chk-257)).
  `7.0 / 2` is the float `/`, since one operand is no integer literal.
* **CHK-270** A `<<` or a `>>` whose count is a constant outside 0 to 31 is the normal error `shift-out-of-range`, whatever it shifts; a count computed at run time keeps its low five bits (EVAL-86).
* **CHK-310** A **constant** is a literal, an enum value, or a construction, a member, a logical operator or a call of a `@pure` builtin whose operands are all constants.
  It is judged on the flat tree of each entry point and each test, where a literal argument of an inlined call stands for its parameter (EVAL-48), and folded by the abstract machine.
  A `let` holds no constant, even of one: WGSL folds what it creates the shader from, and a local is no part of that.
  WGSL refuses at shader creation what CHK-270, CHK-311 and CHK-312 refuse, so they hold on every target, and an entry point is written for all of them or none (EMIT-13).
* **CHK-311** A call with no value is the normal error `constant-without-value`.
  That is an integer `/` or `%` by a constant with a zero component, whatever it divides, and a call of constants that EVAL-84 or EVAL-87 leaves without a value.
* **CHK-312** A call of constants whose value its type cannot hold is the normal error `constant-not-representable`, which WGSL folds exactly and refuses.
  That is an `int` sum, difference, product, negation, absolute value or left shift outside the `int`s, and a `uint` shifted left past its top bit.
  It is also a negative `int` converted to `uint`, and a `float` result that is infinite or NaN.
  A `uint` sum, difference and product wrap, as WGSL folds them.
* **CHK-313** A `/` or a `%` whose operands are all integer literals is the normal error `literal-needs-type`, although `int` has both ([why](why/checking.md#chk-313)).
  So `1 / 3` is an error, and `1.0 / 3`, `(1 as int) / 3` and a division of two `int` locals are not.
* **CHK-71** No matching candidate is the normal error `no-matching-overload`, and its detail spells the call with its argument types and says why each candidate did not match.
* **CHK-72** Matching candidates without a best are the normal error `ambiguous-overload` ([why](why/checking.md#chk-72)).
* **CHK-73** The best matching candidate is the call's target, and its return type is the call's type.
* **CHK-256** The spelling is checked on the target: `a.foo` whose target is no property, and `a.foo(…)` whose target is a property, are the normal error `call-spelling`.
  Its detail gives the other spelling.
  A free call may reach a property: `length(v)` is legal where `length` is a property ([why](why/checking.md#chk-256)).
* **CHK-74** A call of a function that is not `@builtin` resolves like any other, and it is inlined, by CHK-127 to CHK-132.
* **CHK-75** A call whose callee names a struct is a call of that struct's overload set: its synthesized constructor and the functions of its name (CHK-240).
* **CHK-76** A call whose target is a synthesized constructor is a **construction**, and its type is the struct.
* **CHK-77** A splat argument `..value` in a call of a struct's name stands for the fields of `value`, in order, as positional arguments.
  `value` is of a struct type with fields, or it is `type-mismatch`.
* **CHK-78** A splat in any other call, a call of a local and type arguments are `unsupported-yet`.
* **CHK-79** A callee that names a binding is `wrong-kind-of-name`.
* **CHK-80** `and`, `or` and `not` are no functions: CHK-116.
* **CHK-195** `x as T` is a call of the `@operator("as")` function whose one parameter is the type of `x` and whose result is `T`.
  The result takes part in the match, since the overloads of `as` differ in it; no such function is `no-matching-overload`.
* **CHK-196** `x as T` where `x` already has the type `T` is `x`.
* **CHK-197** The prelude converts between `float`, `int` and `uint` of one width, and nothing else ([why](why/checking.md#chk-197)).
  A float whose truncation the integer holds becomes that integer, truncated toward zero.
  A float out of the integer's range, and a NaN, become a value the language does not specify, and it may differ between targets.
  Between `int` and `uint` the bits stay.
  The interpreter's choice of the unspecified value is saturation at the integer's bounds, and 0 for a NaN.

```sgl
let n = normalize p.normal
let key = saturate dot(n, normalize vec3(0.45, 0.8, -0.4))
let lit = p.color * (0.25 + 0.8 * key + 0.25 * fill)
let color = float4(..lit, 1.0)
```

## Literals

* **CHK-81** A tuple or an object literal where a type `T` is expected converts to `T`: it is a call of `T`'s name whose arguments are its elements ([why](why/checking.md#chk-81)).
  A tuple's elements are positional arguments and an object's are named ones, and a round literal may hold both.
* **CHK-82** A type is expected at an argument, the value of a `return`, a `yield` of a property with `-> T`, a `let` with a type, an assignment and a field's value.
  A tuple or an object literal anywhere else is `unsupported-yet`.
* **CHK-83** Only a literal written where the type is expected converts, and a value of a structural type converts to nothing.
* **CHK-84** The chain of a converted literal is 1 longer than the longest chain of its call, and an element that is itself a literal converts to the parameter of the candidate it meets.
  A candidate one of whose literal elements has no target by CHK-73 does not match.
* **CHK-85** The target of a conversion to `T` returns `T`, or it is the normal error `literal-conversion-result`; a function of `T`'s name that returns another type may still be called by name.
* **CHK-86** A shorthand element and a splat of an object are `unsupported-yet`.

```sgl
struct span2:
    lo: float
    hi: float = 1.0

fun width(s: span2) -> float => s.hi - s.lo

fun f() -> float:
    let s: span2 = (0.25, 0.75)
    return width({lo = 0.5}) + width(s)
```

## Entry points

* **CHK-87** A function that carries `@vertex` or `@pixel` is an **entry point** of that stage; one that carries two stages is the normal error `invalid-entry-point`.
  `@compute`, `@geometry`, `@tessellation_control` and `@tessellation_evaluation` make an entry point too, each of its own stage.
  So do the ray-tracing stages of [CHK-326](#ray-tracing).
* **CHK-88** A vertex or pixel entry point takes at most one parameter without a stage input's attribute, its **stage struct**, which is of a struct type with fields and comes first.
  A `@pixel fun` takes one; a `@vertex fun` may take none, and then draws from no vertex buffer.
* **CHK-89** The stage struct of a `@vertex fun` is of a `@vertex struct`.
* **CHK-271** A parameter marked with a stage input's attribute is a **stage input**: a value the GPU hands the invocation ([why](why/checking.md#chk-271)).
  Each is of one stage and one type, and an entry point takes each at most once, after its stage struct:

| attribute | stage | type |
|---|---|---|
| `@vertex_index`, `@instance_index` | vertex | `int` |
| `@is_front_facing` | pixel | `bool` |
| `@sample_index` | pixel | `int` |
| `@sample_mask` | pixel | `uint` |
| `@primitive_id` | pixel, geometry, tessellation control and evaluation | `int` |
| `@domain_location` | tessellation evaluation | `float3` or `float2` (CHK-306) |
| `@thread_id`, `@local_thread_id`, `@workgroup_id` | compute | `int3` |
| `@local_thread_index` | compute | `int` |
| `@launch_id`, `@launch_size` | every ray-tracing stage (CHK-327) | `int3` |

  A `@compute fun` takes stage inputs alone.
  `vertex_index` and `instance_index` count from the draw's first vertex and first instance on every target.
* **CHK-273** `@interpolate(kind, sampling)` on a member of the struct the vertex stage returns says how the member crosses to the pixel stage ([why](why/checking.md#chk-273)).
  The kind is `.perspective`, the default, `.linear` or `.flat`; the sampling is `.center`, the default, `.centroid` or `.sample`, and `.flat` takes none.
  An `int` or `uint` member, and a vector of them, crosses only `.flat`, or the entry point is `invalid-entry-point`, and no member that crosses no stage edge carries `@interpolate`.
  A flat member takes the value of the primitive's first vertex.
* **CHK-275** `@format(.case)` on a member of a `@vertex struct` names the `sg::vertex_attribute_format` its bytes are, and never a pipeline setting ([why](why/checking.md#chk-275)).
  The format decodes into the member's type, or it is `type-mismatch`: `.rgba8_unorm` into a `float4`, `.rgba8_uint` into a `uint4`.
  A member without one reads the format its type has at full width: `float3` is `vec3f`, `uint` is `u32`.
* **CHK-277** An entry point whose inlined body reaches a `discard` is a `@pixel fun`, or it is `stage-not-allowed` at the `discard` ([why](why/checking.md#chk-277)).
  A path that ends in a `discard` needs no value, as one that ends in `return` does.
* **CHK-276** A member of a `@pixel struct` marked `@depth` is the pixel's depth, a `float`, and one marked `@sample_mask` the samples it writes, a `uint` ([why](why/checking.md#chk-276)).
  Neither is a color target, so neither takes a location or a `color_targets` entry, and a struct holds one of each at most.
  `@depth(.greater_equal)` and `@depth(.less_equal)` promise that the written depth only moves that way from the rasterized one.
  A pipeline whose pixel stage writes depth has a `depth_stencil_format` other than `.undefined`, or `invalid-pipeline`.
* **CHK-274** A pixel stage that takes a member interpolated `.sample` runs once per sample, and needs `sample_rate_shading` of a device.
* **CHK-272** `@primitive_id` needs `primitive_index` of a device and `@sample_index` needs `sample_rate_shading`, as a binding member needs its feature (CHK-261).
  The feature is the pixel stage's alone: the geometry and tessellation stages have the primitive's index wherever they have the stage.
* **CHK-90** A `@vertex fun` returns a struct with at most one field that carries `@position`, and that field is of the type `hpos4`.
  The struct that reaches the rasterizer carries exactly one, which CHK-307 asks of the pipeline.
* **CHK-91** A `@pixel fun` returns a `@pixel struct`.
* **CHK-92** An entry point is neither `@builtin` nor `@operator`.
* **CHK-173** `@per_instance` and `@stream(name)` are attributes of a struct field, recorded on the member; `@stream` takes one bare name.
* **CHK-208** `@stages(.pixel)` on a function, builtin or not, names the stages it may be reached from, each an enum case of a stage.
  The cases are `.vertex`, `.tessellation_control`, `.tessellation_evaluation`, `.geometry`, `.pixel` and `.compute`.
  The ray-tracing stages are cases too: `.raygen`, `.miss`, `.closest_hit`, `.any_hit`, `.intersection` and `.callable`.
  A function without it may be reached from every stage, and any other argument is `invalid-attribute-arguments`.
* **CHK-193** An entry point whose inlined body reaches a function whose `@stages` leaves out the entry point's stage is `stage-not-allowed`, at that call.
  It is judged per entry point once everything is inlined, since a function in between says nothing about where it is reached from.
  `sample` without a `level` is `@stages(.pixel)`: its level comes from derivatives, which only a pixel stage has on every target.
* **CHK-93** Breaking one of CHK-88 to CHK-92 is `invalid-entry-point`, and its detail names the rule.
* **CHK-267** `@expect(footprint = "slot: access, ...")` on an entry point pins its [footprint](../bindings.md#footprint): each touched slot once, as `read`, `write` or `read write`, in any order.
  A footprint that differs is `unmet-expectation`, whose detail spells the one the code has.
  On a function that is no entry point, or with any other argument, it is `invalid-attribute-arguments`.

## Features

A feature is what a device may lack, so using one makes a shader non-portable on purpose.
[bindings.md](../bindings.md#features) lists the forms each one grants.

* **CHK-258** A `require` names features as `sg::feature` names them, and only those a shader can use:
  `binding_arrays`, `extended_image_formats`, `readwrite_image_formats`, `multisampled_array_textures`, `ray_query` and `raytracing_pipeline`.
  The stages and stage inputs a device may lack add `primitive_index`, `sample_rate_shading`, `geometry_shader` and `tessellation_shader`.
  Any other name is the normal error `unknown-feature`, and its detail lists the names.
* **CHK-259** A `require` at file scope grants its features to everything in the file ([why](why/checking.md#chk-259)).
* **CHK-260** A `require` in a binding grants its features to that binding's members.
* **CHK-261** A binding requires what its own `require` lines name and what its members use, and an entry point that lists it needs all of that of a device ([why](why/checking.md#chk-261)).
* **CHK-262** An entry point declares a feature by a `require` of its file, of a binding it lists, or among the lines of its own body ([why](why/checking.md#chk-262)).
  A `require` inside a nested block is `unsupported-yet`.
* **CHK-263** What an entry point needs of a device is what it uses, never what it merely may use ([why](why/checking.md#chk-263)).
  It needs what the bindings it lists require, its stage inputs (CHK-272), a member it takes per sample (CHK-274) and its stage itself (CHK-301, CHK-304, CHK-306, CHK-326).
  It needs what the builtins its inlined body calls need, by CHK-322.
  It is judged once every body is checked, and a use is counted wherever it stands, reached or not.
* **CHK-322** A builtin's record may name the features a call of it needs, and an entry point whose inlined body reaches such a call needs them too ([why](why/checking.md#chk-322)).
  It declares them as any other, by CHK-262, and one it does not is `feature-not-declared` with a note at the call.
  So a body's `require` that such a call needs is used, which CHK-265 judges once every entry point is flattened.
  The prelude's `trace` is how a program reaches one: its steps need `ray_query`.
* **CHK-264** A feature an entry point needs and does not declare is the normal error `feature-not-declared` at its name, with a note at each listed binding that needs it.
* **CHK-265** A `require` in a body that is not the declaration an entry point needs is the warning `unused-require`, and so is a second `require` of a feature in one body.
  A `require` of a file or of a binding is never unused: each declares an intent, whether anything uses the feature or not ([why](why/checking.md#chk-265)).

```sgl
require extended_image_formats

binding post:
    dst: out image_2d[.r8_unorm]
```

A `require` in a test's body that nothing in it uses is `unused-require`, and a name that is none is `unknown-feature`.

```sgl
@expect(warning = "unused-require") test:
    require ray_query
    1 == 1

@expect(error = "unknown-feature") test:
    require raytracing
    1 == 1
```

## Pipelines

[pipelines](../pipelines.md) is the model; these are its rules.

* **CHK-174** A `pipeline` declares a symbol of the module scope, named `pipeline` where it has no name of its own; it shares that scope with the file's functions (CHK-12).
* **CHK-175** Its stages are the settings `vertex` and `pixel`, each the name of one entry point of that stage; in the short form each entry point takes the stage its attribute names.
  A short form's settings block names no stage.
  A pipeline has one vertex stage and at most one pixel stage, and a compute entry point is in none.
* **CHK-176** Every other setting assigns one field of the prelude's `raster_pipeline_description`, and the settings apply in source order, each over the ones before it.
* **CHK-177** The left side of a setting is a path of field names; its first name, where it is no field of the description, stands for the one field of that name below it.
  A name that is no field anywhere, or two, is `invalid-pipeline`, and the detail names both paths.
* **CHK-178** A value is `true` or `false` for a `bool`, a number literal for an `int` or a `float`, and a case for an enum (CHK-152).
  A paren literal writes a struct whole and names each of its fields once, or it is `missing-field`, `unknown-field` or `duplicate-field`.
  An `int` is held to the range sg keeps its field in: a stencil mask is 0 to 255, `sample_count` a power of two from 1 to 64, and `patch_control_points` 0 to 32.
* **CHK-179** Under `color_targets` stands one entry per member of the pixel stage's `@pixel struct`; a setting whose path names no member there is one per member.
* **CHK-180** `.host` stands only for a target's `format`, the `depth_stencil_format` and the `sample_count`, and `.none` only for a target's `blend`.
* **CHK-181** An attribute of a `@vertex` or `@pixel` entry point, of a `@vertex struct` or of a `@pixel struct` is a setting when its name alone is one field.
  A geometry or tessellation entry point takes no setting yet, and one there is `unsupported-yet`.
  On a member of a `@pixel struct` it is one of that target's fields.
  Such an attribute takes one value.
  One whose name is two fields is `invalid-pipeline`, and the detail names both and says to set it in the pipeline, since an attribute has no path.
* **CHK-182** A pipeline's settings apply in this order: the attributes of its vertex input, then of its `@pixel struct` and its members.
  Then the attributes of its vertex stage and of its pixel stage, in that order, then its own.
  Two sources of one step that set one field differently are `invalid-pipeline`, unless the pipeline sets that field itself.
  A part and a field inside it count as one field here, so `blend = .none` meets every field of another source's blend, and the pipeline's own `blend = .none` settles them.
* **CHK-183** What a stage returns has the members the next stage takes: as many, with the same names and types, in the same order, and `@position` on the same one.
  CHK-307 says what each stage between the vertex and the pixel stage takes and returns.
* **CHK-308** A pipeline with a geometry stage whose pixel stage takes `@primitive_id` is `invalid-pipeline`.
  D3D and vulkan hand the pixel stage the id only where the geometry stage writes it, which an SGL geometry stage cannot yet.
  The geometry stage takes `@primitive_id` itself and passes it on as an `@interpolate(.flat)` int member of what it appends.
* **CHK-184** Its stages' binding lists, `@inline` bindings left out, name the same binding at every position they share.
  The longest is the pipeline's layout, and the stages list one `@inline` binding at most.
* **CHK-185** Every target has a format at the end: a case other than `.undefined`, or `.host`.
* **CHK-186** `@compute` on a pipeline is `unsupported-yet`; `@raytracing` is [CHK-331](#ray-tracing).
* **CHK-187** Breaking one of CHK-175 to CHK-185 is `invalid-pipeline`, unless a rule names another kind, and its detail says what broke.

## Arrays

`T[N]` is N values of `T`, a value like a struct: copied where it is passed or assigned, and read and written by element.

* **CHK-285** A square group applied to a complete type in a type position makes an array of it: `float[5]`, `polygon[3]`, `texture_2d[float4][64]`.
  Every length is an `int` literal or an `int` `const`, at least 1, or `invalid-constant-argument`.
  An array of arrays is one group, outermost first, `float[3, 5]` for three arrays of five; two groups in a row are `wrong-kind-of-name` ([why](why/checking.md#chk-285)).
* **CHK-286** `T[]`, a group with no length, is an array whose length the host binds, which only a binding member may be; anywhere else it is `wrong-kind-of-name`.
* **CHK-287** `xs[i]` is an element of an array, and `xs[i, j]` is `xs[i][j]`: one `int` index per dimension, at most as many as the array has.
* **CHK-309** An index that is an `int` literal or names an `int` `const` lies in `0 ..< N` for its dimension's length `N`, or it is `invalid-constant-argument`.
  An index computed at run time is not judged here.
* **CHK-288** `xs.length` is an array's length, an `int` constant, and the one member an array has; it is never assigned.
* **CHK-289** `T[N].filled(v)` is an array of `T[N]` whose every element is `v`, which converts to `T` as an argument does.
* **CHK-290** A square literal converts to the array type expected where it stands, with exactly as many elements, each converting to the element type.
  Where no array is expected it is an array of its first element's type, and every other element converts to that.
  It holds at least one element, and no name and no splat.
* **CHK-291** An array in a constant block, in a buffer's element or in a struct that crosses a stage edge is `unsupported-yet`, at any depth of the structs holding it.
  The detail names the path down to it.
  A tessellation factor is an array by design (CHK-305), and workgroup memory has no host side, so neither counts.
* **CHK-299** A binding member `T[N]` of a texture, an image or a buffer is a **binding array**, which needs `binding_arrays`.
  Its `N` is at least 2, or it is `invalid-constant-argument`: one resource is a plain member.
  It is read by element alone, `name[i]`, and naming it whole is `wrong-kind-of-name`; an element is the resource, which only a builtin takes.
  `T[]`, an array of samplers and one of more than one dimension are `unsupported-yet`.
  An array whose innermost element is a value is no binding array, whatever its dimensions: CHK-291 is what judges it.
  An access word stands before it and qualifies its element: `out image_2d[.rgba8_unorm][4]`.
* **CHK-300** An index into a binding array that the uniformity pass cannot prove the same in every invocation is `non-uniform-index` unless it is `nonuniform i` ([why](why/checking.md#chk-300)).
  `nonuniform i` is its argument unchanged, and on an index the pass proves uniform it is the warning `needless-nonuniform`.
  It stands only as the whole index into a binding array, `textures[nonuniform i]`; anywhere else, a `let` or an index into a value array included, it is `wrong-kind-of-name`.

## Workgroup memory

* **CHK-292** A `@workgroup` binding's members are values its workgroup shares; a resource or a sampler block in one is `wrong-kind-of-name`.
  An array stands in one, since workgroup memory has no host layout, and a member of one is a place a shader assigns.
  `@inline` or `@no_padding` together with `@workgroup` is `invalid-attribute-arguments`.
* **CHK-293** A `@workgroup` binding whose members take more than 16384 bytes, laid out as WGSL lays out workgroup variables, is `invalid-attribute-arguments`.
* **CHK-294** An entry point listing a `@workgroup` binding is a compute stage, and all it lists fits the same 16384 bytes, or it is `invalid-entry-point`.
* **CHK-295** A test uses a `@workgroup` binding, and calls a function listing one, without listing it: the run holds its own.

## Atomics

* **CHK-296** `atomic[T]` is an atomic of `uint` or `int`, and of any other `T` `wrong-kind-of-name`.
  It is the element of a `mut buffer` or a member of a `@workgroup` binding, arrays of them included; in a read-only buffer or a constant block it is `wrong-kind-of-name`.
  Its builtins are `@stages(.pixel, .compute)`.
* **CHK-297** An expression of an atomic's type stands only as a builtin's argument; anywhere else, and as the place of an assignment, it is `wrong-kind-of-name`.
  A local, a parameter or a field of an atomic's type is `wrong-kind-of-name` as well.

## Uniformity

A barrier waits for every thread of its workgroup, and a derivative compares a pixel with the other three of its quad.
So each stands where every invocation of its group arrives together, which the check pass judges once every call is inlined.
It judges the tree an emitter prints, and must be sound for every target: what it accepts reaches each call in uniform control flow.
Its rules start from WGSL's, but refusing all that Tint refuses is no goal; where Tint refuses a sound program, the WGSL text silences it (EMIT-103).

* **CHK-282** A barrier, and a builtin that takes derivatives implicitly, in **non-uniform control flow** is `non-uniform-control-flow`, at the call.
  A note names the branch or the exit that made the flow so, and what the branch tested.
  `sample` without a `level` or gradients, `sample_compare` without a `level`, `ddx` and `ddy` take derivatives.
* **CHK-283** A value is **non-uniform** where it comes from a stage input other than `@workgroup_id`, from the stage struct, from a `mut` buffer or a `mut` image,
  from a non-uniform value, or from a local set anywhere in non-uniform control flow.
  A `mut` buffer or image is so whether it is named directly or as an element of a binding array.
  Every read of workgroup memory is non-uniform, whatever was stored to it, and so is the result of every atomic.
  Every other value is uniform: a literal, a `const`, a member of a constant block, and an element of a read-only buffer at a uniform index.
* **CHK-284** Control flow is non-uniform inside an `if`, a `case` or a loop whose condition is non-uniform, and the right side of an `and` or an `or` whose left side is.
  It stays so after an `if` or a `case` one of whose sides leaves in non-uniform control flow, and for the rest of the entry point after such a `return`.
  A loop some invocations leave early, by a `break`, a `continue` or a `return` in non-uniform control flow, is non-uniform throughout and after it.
  A `while` condition is tested again before every iteration, so such a loop's condition runs in non-uniform control flow too.
  An inlined function's early `return` is such an exit of the block it became.
  A `discard` changes nothing, as in WGSL, where the pixel goes on as a helper of its quad.

## Geometry and tessellation stages

Two optional stages stand between the vertex and the pixel stage, each an entry point of its own and each a feature a device grants.
HLSL writes them on dx12 and vulkan; WebGPU and Metal have neither, so WGSL and MSL refuse by the feature.

* **CHK-301** `@geometry(max_vertices = N)` makes an entry point of the geometry stage, which needs `geometry_shader`.
  `N` is an `int` literal from 1 to 256, and the entry point returns nothing.
  `N` vertices of `T`, the stream's struct, carry at most 1024 scalars: `hpos4` and `float4` count 4, `float3` 3, and an array its length times its element's.
  Those are what vulkan guarantees and what D3D allows, and breaking the second is `invalid-entry-point`.
* **CHK-302** Its first parameter is an array of the struct the stage before it returns, one primitive long.
  A primitive is 1 vertex for a point, 2 for a line, 3 for a triangle, 4 for a line with adjacency and 6 for a triangle with adjacency.
  Stage inputs follow it, `@primitive_id` among them, and its last parameter is `mut point_stream[T]`, `mut line_stream[T]` or `mut triangle_stream[T]`.
  `T` is the struct it hands the pixel stage, and a stream is no value: only the entry point's parameter is one.
* **CHK-303** `s.emit(v)` appends the vertex `v`, which converts to `T`, and `s.end_strip()` ends the strip being appended; each is a geometry stage's alone.
* **CHK-304** `@tessellation_control(partitioning = p, winding = w)` makes an entry point of the tessellation control stage, which needs `tessellation_shader`.
  `p` is `.integer`, `.fractional_even` or `.fractional_odd`, and `w` is `.clockwise` or `.counter_clockwise`.
  `w` is the patch's winding: the triangles the tessellator makes wind as the corners do, where the evaluation stage weighs the control points in order by the domain location.
  A triangle patch evaluated as `patch[0] * uvw.x + patch[1] * uvw.y + patch[2] * uvw.z` of counter-clockwise corners is `.counter_clockwise`.
  So is a quad patch of corners counter-clockwise from its first, blended by `uv.x` from the first to the second and by `uv.y` from that edge to the opposite one.
  Power-of-two partitioning is none of them, since vulkan lacks it.
  Its first parameter is the patch: an array of from 1 to 32 of the struct the vertex stage returns.
  Stage inputs follow it, and it returns a **factors struct**; no other parameter stands.
  A repeated argument is `invalid-attribute-arguments`.
* **CHK-305** A factors struct has exactly one `@edge_factors` member, `float[2]`, `float[3]` or `float[4]`, which makes its domain isolines, triangles or quads.
  It has one `@inside_factors` member, `float` for triangles and `float[2]` for quads, and none for isolines; any other member reaches the evaluation stage as it is.
* **CHK-306** `@tessellation_evaluation` makes an entry point of the tessellation evaluation stage, which needs `tessellation_shader`.
  Its first parameter is the patch, from 1 to 32 control points as the control stage takes it.
  Then it takes the control stage's factors struct, and `@domain_location`: a `float3` for triangles and a `float2` otherwise.
  Stage inputs stand anywhere after the patch.
  It returns the struct the stage after it takes.
* **CHK-307** A pipeline names the stages as `geometry = f`, `tessellation_control = f` and `tessellation_evaluation = f`; the two tessellation stages come together.
  Each stage takes what the one before it returns, by CHK-183's rule, and the struct that reaches the rasterizer carries exactly one `@position`.
  The patch's length is `patch_control_points` and makes the topology `.patch_list`, so naming either as a setting is `invalid-pipeline`.
  A pipeline without the tessellation stages draws no patches, and sets neither but `patch_control_points = 0`.
  A geometry stage's primitive is the one the pipeline assembles: the topology's family, or the tessellator's lines for isolines and triangles otherwise.
  A primitive with adjacency is `unsupported-yet` in a pipeline, since sg has no topology that assembles one.

## Ray tracing

[raytracing](../raytracing.md) is the model, with the vocabulary of `prelude/raytracing.sgl`; these are its rules.

### What a trace runs against

* **CHK-320** `acceleration_structure[.geometry]` is a resource type, whose one argument is `.triangles`, `.procedural` or `.mixed` ([why](why/checking.md#chk-320)).
  Without it, or with any other, it is `wrong-kind-of-name`.
  In the program's file it needs `ray_query`, or `raytracing_pipeline` where the file or its binding grants that one, as CHK-201 needs a feature of a form.
* **CHK-325** A builtin whose record takes the acceleration index is handed, past its signature, the position of its argument among the entry point's acceleration members.
  The members are counted across the entry point's binding list in list order, and the argument is a binding member itself.
  It is the emulated trace's root: the index into the roots sg binds per dispatch (EMIT-135).

### Stages

* **CHK-326** `@raygen`, `@miss`, `@closest_hit`, `@any_hit`, `@intersection` and `@callable` each make an entry point of a **ray-tracing stage**, which needs `raytracing_pipeline`.
  Each takes what its stage is handed, and returns what it gives back, by the table below; breaking it is `invalid-entry-point`, and its detail names the stage's shape.
  A parameter of any type the table does not name is `invalid-entry-point` too.
* **CHK-327** `@launch_id` and `@launch_size` are stage inputs of every ray-tracing stage, both `int3`: the ray of the launch it runs for, and the launch's size.
* **CHK-328** A ray-tracing stage's **payload** is its one `mut` parameter, of a struct type: the caller's place, which the stage reads and writes.
  A payload of another type is `invalid-entry-point`.
* **CHK-342** An `@intersection` is handed its box alone, and no payload ([why](why/checking.md#chk-342)).
  It returns `report[A]`, whose `A` is a struct of the program; a builtin type there is `invalid-entry-point`.
  A procedural hit or candidate a closest hit or an any hit takes carries the attributes the target hands the stage.

| stage | takes, besides stage inputs | returns |
|---|---|---|
| `@raygen` | nothing | `void` |
| `@miss` | its payload, and at most one `ray` | `void` |
| `@closest_hit` | one `triangle_hit` or `procedural_hit[A]`, and its payload | `void` |
| `@any_hit` | one `triangle_candidate` or `procedural_candidate[A]`, and its payload | `hit_decision` |
| `@intersection` | one `procedural_box` | `report[A]` |
| `@callable` | its parameter, as a payload | `void` |

### Traces and calls

* **CHK-329** `trace(world, r, set.ray, mut p)` whose third argument names a ray type of a ray set is a **trace of a ray type**, and no call of the prelude's `trace`.
  `world` is an acceleration structure and `r` a `ray`, or `type-mismatch`; a name the set has no ray type of is `unknown-member`.
  The payload is marked `mut`, or `no-matching-overload`, and it is a place (CHK-316) of exactly the ray type's payload, or `type-mismatch`.
  `flags`, a `ray_flags`, and `mask`, an `int`, may follow by name, and any other argument is `no-matching-overload`.
  It gives `void`, and is the target's trace with the ray type's position in its set as the ray contribution and the miss index, and the set's size as the multiplier.
  Its builtin is `@stages(.raygen, .closest_hit, .miss)`, so a trace from any other stage is `stage-not-allowed` (CHK-193).
* **CHK-343** `callables name = (…)` is a **callables table**: `@callable` entry points of one parameter type, and `.host` last for the host's ([why](why/checking.md#chk-343)).
  A table lists at least one callable, a block of settings is none, and `.host` anywhere but last is `invalid-pipeline`.
  So is a callable of another parameter type, whose detail names both.
  The tables of a module pack in declaration order, so a table that takes `.host` is the module's last, or `invalid-pipeline`.
* **CHK-344** `table[i](mut p)` calls the callable at the run-time index `i` of a callables table, an `int`.
  It takes one argument, marked `mut`, or `no-matching-overload`, which is a place of exactly the table's parameter type, or `type-mismatch`.
  It gives `void`, and is the target's call of the callable at the table's place in the module's section plus `i`.
  Its builtin is `@stages(.raygen, .closest_hit, .miss, .callable)`.

### Declarations of a pipeline

* **CHK-330** A `hit_group name for set:` is one row of a ray-tracing pipeline's table, for the ray set it names.
  A name that is no ray set is `invalid-pipeline`, and one that names nothing `unknown-name`, in a hit group and in a pipeline's `rays` alike.
  Its settings are `geometry`, `intersection`, and at most one record per ray type of its set, `ray = (closest_hit = f, any_hit = g)`, either left out.
  `geometry` is `.triangles`, the default, or `.procedural`; a procedural group names an `@intersection`, and a group that names one is procedural.
  A record's shaders are entry points of the stage their slot names, and each takes the payload of its record's ray type.
  They take what the group's geometry hits, a triangle's or a procedural primitive's, and a procedural group's the `A` its intersection reports.
  Breaking any of these is `invalid-pipeline`, and its detail says what broke.
* **CHK-331** A `@raytracing pipeline` names its shaders in a block of settings, and its short form is `invalid-pipeline` ([why](why/checking.md#chk-331)).
  `rays` names its ray set and `raygen` its `@raygen` entry point, and it has both.
  `miss.<ray>` names the `@miss` of a ray type of the set, which takes that ray type's payload; a ray type may have none, and none has two.
  `hit_groups` is a hit group, `.host`, or a round list of them, each a group for the pipeline's ray set, with `.host` last.
  `max_recursion_depth` is an `int` literal from 1 to 31, which a pipeline with `.host` declares and any other does not.
  Every shader it names lists the same binding at every position its lists share, `@inline` bindings left out, and they list one `@inline` binding at most.
  Any other setting, and breaking any of these, is `invalid-pipeline`.
* **CHK-332** A pipeline's **trace graph** has an edge from each ray type to every ray type its miss, or its closest hit in a listed group, traces ([why](why/checking.md#chk-332)).
  A cycle in it is `recursive-trace`, and the pipeline fails.
  Its depth is the longest chain from a ray type the raygen traces, and at least 1.
  Without `.host`, the depth is the pipeline's `max_recursion_depth`; with it, a depth past the declared one is `invalid-pipeline`.

### What metal adds

* **CHK-345** Each record of a procedural hit group is also one **traversal entry point** on metal, `sgl_<group>_<ray>`: its intersection, fused with the record's any hit.
  A report before the ray's `t_min` or past its current `t` is no hit, and one within it is what the any hit decides, or accepted without one.
  The traversal takes the payload that any hit writes.
  Where a record has no closest hit, the module has one more entry point, `sgl_empty_closest_hit`, owned by a ray-tracing entry point of the program, which does nothing.

```sgl
rays path_rays:
    surface: radiance
    occlusion: shadow

hit_group textured for path_rays:
    surface = (closest_hit = shade, any_hit = cutout)
    occlusion = (any_hit = shadow_cutout)

@raytracing pipeline path:
    rays = path_rays
    raygen = primary
    miss.surface = sky
    miss.occlusion = open_sky
    hit_groups = (textured)
```

## The flat tree

* **CHK-94** The pass has two results: side tables over the untouched ASTs, and one **flat tree** per entry point.
* **CHK-95** The side tables give each expression its type and its **target**: a local, a parameter, a symbol, the chosen overload, a constructor, a field or a binding member.
* **CHK-96** An expression in a type position has the type it names.
* **CHK-97** A flat tree holds locals, structured control flow, member access, binding members, constructions, literals and calls of `@builtin` functions ([why](why/checking.md#chk-97)).
* **CHK-107** The pass writes the **structured form**, whose meaning is [evaluation.md](evaluation.md); `once` and a bare `break` are the core form's alone.
* **CHK-108** A flat call records whether its callee is `@pure`, so a reader of the tree needs no symbol to know whether a call has an effect.
* **CHK-98** Every flat expression has a type, and none has the error type.
* **CHK-99** Every flat node names the AST node it came from and the chain of call sites it was inlined through, outermost first.
  The chain is empty for a node of the entry point's own body.
* **CHK-100** An entry point whose signature or body reported an error has no flat tree, and neither has one that reaches such a function, by CHK-132.
* **CHK-101** A flat entry point records its stage, its name, its parameter's struct, its result struct, and the bindings of its binding list in the order written.
* **CHK-102** A converted literal is its call, and a construction has one value per field, in field order (EVAL-19).
* **CHK-103** A splat is spread into one member access per field.
  A splatted value that is no local is bound to a temporary local where its first member stands, so it is evaluated once and no earlier than written.
* **CHK-104** Every name an emitter writes comes from one **mint**, which hands out a desired name when it is free and `name_1`, `name_2`, … otherwise ([why](why/checking.md#chk-104)).
* **CHK-105** An entry point keeps its name, and every module-level name is taken in the mint before the first local is minted.
* **CHK-213** An entry point whose flat tree the pass cannot write, though nothing it reaches reported an error, is `unsupported-yet` at its name.
  A gap of the pass is never a silent loss of the entry point.
* **CHK-268** A flat tree nests at most 40 levels, and an entry point whose tree nests deeper is `nesting-too-deep` and has no flat tree.
  A level is an operand, the body of a block expression, an expression a statement holds, and a statement list inside a statement; every call counts as inlined.
  A top-level `let x = a + b + …` is one level for the `let` and one per term, so 40 terms is past the limit.
  This is an implementation limit rather than a rule of the language, and it is expected to rise.

## Control flow

* **CHK-109** The body of an `if` branch, of a `while`, of a `for` and of a `loop:` is a block, and a block is a scope: a local ends where its block ends.
* **CHK-110** A local of an inner block that shadows one of an enclosing block is CHK-53's shadowing: the outer local is visible again once the block ends ([why](why/checking.md#chk-110)).
* **CHK-111** `let mut name = value` introduces a mutable local; a local without `mut`, a parameter and the variable of a `for` are immutable.
* **CHK-112** `place = value` needs `place` to be a mutable local or a member of one, at any depth, or it is the normal error `not-assignable`.
  `value` is of the type of `place`, or it is `type-mismatch`.
* **CHK-113** `place op= value` is `place = place op value`: the operator resolves by CHK-70 to CHK-73, and its result is of the type of `place`.
* **CHK-114** The condition of an `if` and of a `while` is a `bool`, or it is `type-mismatch`.
* **CHK-115** `for name in first ..< end` runs over `int`: both bounds are `int`, and `name` is an immutable `int` local of the body.
  A type on the variable is `int`; a range that is not `..<`, and anything else behind `in`, is `unsupported-yet` ([why](why/checking.md#chk-115)).
* **CHK-116** `and`, `or` and `not` are the language's own: their operands are `bool` and so is their value, and no function declares them ([why](why/checking.md#chk-116)).
* **CHK-117** In a comparison chain each operator resolves over its two neighbours by CHK-70 to CHK-73 and gives a `bool`, and the chain is a `bool`.
* **CHK-118** A `loop:` that is a statement is left by `break`, and one that is a value by `break value`.
  The values of one loop are of one type, which is the loop's; a `break` of the other kind is `type-mismatch`.
* **CHK-119** `break` and `continue` name the innermost loop around them.
* **CHK-120** `return`, `break` and `continue` are statements; one that stands as the result of a `case` arm leaves as it says, and that arm produces no value (CHK-165).
  One that stands as a value anywhere else is `unsupported-yet`.
* **CHK-209** `print value` takes a value of any type; a string is `unsupported-yet`.

```sgl
fun falloff(d: float, steps: int) -> float:
    if d <= 0.0 => return 0.0
    let mut w = 1.0
    for i in 0 ..< steps:
        w *= d
        if w < 0.125 => return 0.0
    return w
```

## Case

* **CHK-153** `case value:` evaluates `value`, the **scrutinee**, and runs the first arm whose pattern matches it.
* **CHK-154** A pattern is an expression of the scrutinee's type, and it **matches** when `scrutinee == pattern` ([why](why/checking.md#chk-154)).
* **CHK-155** The scrutinee is of a type whose `==` resolves by CHK-70 to CHK-73, which an enum's does by CHK-149; one that does not is `no-matching-overload` at the arm.
* **CHK-156** `_` is a pattern that matches anything, and the arm that carries it is the **default**.
* **CHK-157** `a or b` in a pattern is a list of patterns, and the arm matches when any of them does; the `or` of CHK-116 is not involved and its operands are no `bool`s.
* **CHK-158** A pattern is evaluated only where it is reached, so a pattern behind the one that matched is never evaluated ([evaluation](evaluation.md#case)).
* **CHK-159** A `case` is **exhaustive** when it carries a `_`, or when its scrutinee is an enum and its patterns are constant cases that together name every case of it.
* **CHK-160** A `case` that is not exhaustive is the normal error `non-exhaustive-case`; its detail names the cases nobody matched, or says that a `_` is needed ([why](why/checking.md#chk-160)).
* **CHK-161** Two constant patterns of one `case` that name one case is the normal error `duplicate-case-pattern`, at the later arm.
  Two patterns that are equal expressions are not compared: what they hold is known at run time and not here.
* **CHK-162** An arm behind a `_` never runs: it is the warning `unreachable-code`, once per `case`.
* **CHK-163** A `case` that stands as a statement has arms that produce no value.
* **CHK-164** A `case` that stands as an expression has one type, and every arm produces a value of it or exits.
* **CHK-165** An arm **exits** when its body exits by CHK-123, which is what lets `_ => return black` stand where a value is expected.
* **CHK-166** The type of a `case` expression is the one the context expects where there is one, and the type of the first arm that produces a value otherwise.
* **CHK-167** An arm whose value is of another type is `type-mismatch`, reported at the arm.
* **CHK-168** An arm of a `case` expression that neither produces a value nor exits is the normal error `missing-value-in-arm`.
* **CHK-169** A `case` statement whose every arm exits makes its own statement list exit, by CHK-123.
* **CHK-170** An arm's body is a block named after the `case`, as CHK-133 names the block of an inlined call.
  `=> value` leaves it with that value, a `=>:` block leaves it with its `yield`, and the block is EVAL-29's construct in both.

```sgl
fun shade(kind: light_kind, base: float) -> float:
    let weight = case kind:
        .point => 1.0
        .spot or .sun => 0.5
        _ => return 0.0
    return base * weight
```

## Returning

* **CHK-121** A function without `-> T` returns `void`, exactly as one with `-> void` does: its `return` carries no value or a `void` one.
* **CHK-122** An arrow body without a written return type returns what its expression is: the function's return type is the type of that expression.
  A block body without `-> T` still returns `void`, by CHK-121.
* **CHK-123** A statement list **exits** when it holds a `return`, a `break` or a `continue`, an `if` with an `else` whose every branch exits, or a `loop:` that holds no `break` of its own.
  So does an exhaustive `case` (CHK-159) whose every arm exits.
* **CHK-124** A `while` and a `for` never make their list exit, whatever their condition is ([why](why/checking.md#chk-124)).
* **CHK-125** The body of a function that returns a value exits, or it is the normal error `missing-return`.
* **CHK-126** A statement that follows an exit in its list never runs: it is the warning `unreachable-code`, once per list.

This one is `missing-return`: where `x` is at least 0.5, its body ends without a `return`.

```sgl
fun grade(x: float) -> float:
    if x < 0.5 => return 0.0
```

## Calls of the program's functions

* **CHK-127** A call of a function that is no `@builtin` is defined by **substitution**: it means the callee's body, with each parameter standing for its argument ([evaluation](evaluation.md#calls)).
* **CHK-128** Every call is inlined, so no function of the program reaches a target.
* **CHK-129** Each function body is checked once, on its own, whether or not anything calls it ([why](why/checking.md#chk-129)).
* **CHK-130** A function that calls itself, directly or through others, is the normal error `recursive-call`.
  It is reported once per loop, at the call that closes it, and its detail names the loop: `a -> b -> a`.
* **CHK-131** Bindings are an effect: a call needs every binding of the callee's list in the caller's list, or it is `binding-not-listed` at the call.
  So what a function reads is listed by whoever calls it, up to the entry point ([why](why/checking.md#chk-131)).
* **CHK-132** An entry point has a flat tree when its own body and the body of every function it reaches checked without an error, recursion included.
* **CHK-133** An inlined call is a block named after its callee, and two inlines of one function share no local: each gets its names from the mint.
* **CHK-134** `mut self`, a nested function, an anonymous `fun`, and an arrow lambda with a block body are `unsupported-yet`.
  A mut parameter is CHK-315, and a function handed to a parameter of function type CHK-318.

## Generics

A type parameter is opaque where it is declared, so a generic body is checked once, over it.
A call deduces what each parameter stands for, and inlining writes that in its place, so no emitter meets one.

* **CHK-338** `fun f[A, B](…)` declares type parameters, which its signature and its body may name and know nothing of ([why](why/checking.md#chk-338)).
  A value of one is handed on, stored and returned, and nothing else: no field, no operator and no call but one whose parameter is of that type parameter too.
  A type parameter in scope hides every type of its name, and one with a bound, a default or an attribute is `unsupported-yet`.
  An entry point with type parameters is `wrong-kind-of-name`, since the GPU hands it values of known types.
  A call that states its type arguments, `f[float](x)`, is `unsupported-yet` (CHK-78): a call deduces them.
* **CHK-339** `struct name[A]:` declares a **generic struct**, over one type parameter its members may name ([why](why/checking.md#chk-339)).
  Only the prelude declares one: in the program's file, and with more than one type parameter, it is `unsupported-yet`.
  `name[T]` in a type position is an **instance**, and two mentions of one instance are one type; a count of type arguments but one is `wrong-kind-of-name`.
  An instance's members are the template's with `T` for `A`, and a function of the template's type scope serves every instance.
  The bare name in a type position is the template itself, open in its parameter, which a call deduces as it deduces a type parameter.
* **CHK-340** A call of a generic function binds each type parameter from its arguments, then from where the call stands.
  An argument whose parameter's type names a type parameter binds it to the argument's type exactly, and a number to the type it was checked as.
  Two arguments that bind one parameter to two types leave the candidate unmatched, and the call is `no-matching-overload`.
  A function and a literal are bound last, since what they meet depends on the rest: a lambda takes the parameter types the call bound, and its result binds what they left.
  What is still unbound is bound from the type expected where the call stands (CHK-82); a type parameter nothing binds is `no-matching-overload`, whose detail names it.
  Every instance an inlined body names exists before the first entry point is flattened.
* **CHK-341** `undefined()` in the prelude is a value of the type of the parameter it meets, and binds no type parameter ([why](why/checking.md#chk-341)).
  It is a local declared and never assigned, which nobody may read: the interpreter reads it as zeroes, and a target reads what its local holds.
  `report.none()` is the use it exists for: a report with no hit has attributes of the report's type and no value.

```sgl
fun apply[A](x: A, f: (A) -> A) -> A => f(x)

fun twice[A](x: A, f: (A) -> A) -> A => apply(apply(x, f), f)

fun bumped(n: int) -> int => twice(n, x => x + 1)
```

## Inferred results and dropped values

* **CHK-135** A function whose return type is inferred by CHK-122 has its body checked as part of compiling it: for such a function the body belongs to the signature ([why](why/checking.md#chk-135)).
* **CHK-136** So needing such a function from inside its own body, directly or through other inferred functions, is `dependency-cycle` by CHK-18.
  One written return type on the loop makes it the `recursive-call` of CHK-130.
  A call does not need an overload whose parameter types cannot take it, since parameters are known before a result is: an overload set stays usable from inside one of its inferred members.
* **CHK-139** An arrow body whose expression is `void`, such as a call of a function that returns `void`, makes its function return `void` by CHK-122.
* **CHK-137** A call that stands as a statement is evaluated and its value dropped, whatever it calls; any other expression as a statement is `unsupported-yet` ([why](why/checking.md#chk-137)).
  In the flat tree it is an `eval` of the call, and a call of a function that returns `void` is the block of CHK-133 as a statement.

```sgl
fun make_mvp(model: mat4){frame} => frame.proj * frame.view * model
```

## The files of the prelude

* **CHK-138** The prelude has three files, in this order: `builtins.sgl`, `core.sgl` and `raytracing.sgl` ([why](why/checking.md#chk-138)).
  The builtin registry generates the first, and the other two are written by hand.
* **CHK-140** The text of `builtins.sgl` that is checked is generated in memory, and the committed file is byte for byte the same, so a diagnostic's line and column are right in it.
* **CHK-141** Every record of the registry carries its declaration as SGL source, and that text goes through the same parser as any other: the registry has no second signature language.

## Diagnostic kinds

Every kind below is a normal error by [DIAG-4](../syntax/diagnostics.md#rules), except `unreachable-code`, `no-effect` and `unused-require`, which are warnings.
A diagnostic of this pass has a kind, a file, a byte span in that file, and a detail.

| kind | reported by |
|---|---|
| `unsupported-yet` | CHK-8, CHK-61, CHK-213, CHK-237, CHK-291, CHK-299, CHK-307, CHK-314, CHK-321, CHK-333, CHK-338, CHK-339 |
| `duplicate-declaration` | CHK-12, CHK-28, CHK-241 |
| `dependency-cycle` | CHK-18, CHK-136 |
| `unknown-name` | CHK-24, CHK-62, CHK-245, CHK-330 |
| `wrong-kind-of-name` | CHK-24, CHK-54, CHK-79, CHK-237, CHK-247, CHK-199, CHK-200, CHK-202, CHK-203, CHK-205, CHK-279, CHK-285, CHK-286, CHK-292, CHK-296, CHK-297, CHK-299, CHK-300, CHK-317, CHK-320, CHK-338, CHK-339 |
| `missing-type` | CHK-26 |
| `unknown-builtin` | CHK-31 |
| `expected-body` | CHK-32, CHK-236 |
| `opaque-struct-needs-builtin` | CHK-34 |
| `invalid-attribute-arguments` | CHK-36, CHK-39, CHK-204, CHK-208, CHK-211, CHK-212, CHK-220, CHK-231, CHK-267, CHK-292, CHK-293, CHK-301, CHK-304 |
| `binding-not-listed` | CHK-45, CHK-131, CHK-228 |
| `type-mismatch` | CHK-52, CHK-56, CHK-77, CHK-112 to CHK-118, CHK-121, CHK-167, CHK-210, CHK-214, CHK-219, CHK-236, CHK-243, CHK-275, CHK-276, CHK-279, CHK-281, CHK-329, CHK-344 |
| `not-assignable` | CHK-112, CHK-236, CHK-316 |
| `missing-return` | CHK-125, CHK-236 |
| `unreachable-code` | CHK-126, CHK-162 |
| `no-effect` | CHK-225 |
| `recursive-call` | CHK-130 |
| `recursive-trace` | CHK-332 |
| `unknown-member` | CHK-64, CHK-147, CHK-152, CHK-279, CHK-329 |
| `no-matching-overload` | CHK-71, CHK-155, CHK-316, CHK-340, CHK-329, CHK-344 |
| `non-exhaustive-case` | CHK-160 |
| `duplicate-case-pattern` | CHK-161 |
| `missing-value-in-arm` | CHK-168 |
| `needs-feature` | CHK-201, CHK-320 |
| `unknown-feature` | CHK-258 |
| `feature-not-declared` | CHK-264, CHK-322 |
| `unused-require` | CHK-265 |
| `stage-not-allowed` | CHK-193, CHK-277, CHK-298, CHK-329, CHK-344 |
| `ambiguous-overload` | CHK-72 |
| `missing-field`, `unknown-field`, `duplicate-field` | CHK-178 |
| `invalid-entry-point` | CHK-87, CHK-88, CHK-89, CHK-93, CHK-271, CHK-273, CHK-276, CHK-294, CHK-301 to CHK-306, CHK-326 to CHK-328, CHK-342 |
| `nesting-too-deep` | CHK-268 |
| `invalid-pipeline` | CHK-175 to CHK-185, CHK-187, CHK-276, CHK-307, CHK-308, CHK-330, CHK-331, CHK-332, CHK-343 |
| `shadows-unshadowable` | CHK-220, CHK-266 |
| `test-captures-runtime-value` | CHK-228 |
| `test-must-end-in-check` | CHK-226 |
| `unmet-expectation` | CHK-232, CHK-267 |
| `member-name-clash` | CHK-238 |
| `literal-not-representable` | CHK-60, CHK-61, CHK-253 |
| `call-spelling` | CHK-256 |
| `literal-conversion-result` | CHK-85 |
| `literal-needs-type` | CHK-257, CHK-313 |
| `shift-out-of-range` | CHK-270 |
| `constant-without-value` | CHK-311 |
| `constant-not-representable` | CHK-312 |
| `missing-sampler` | CHK-279 |
| `invalid-constant-argument` | CHK-280, CHK-285, CHK-299, CHK-309 |
| `non-uniform-control-flow` | CHK-282 |
| `non-uniform-index` | CHK-300 |
| `needless-nonuniform` | CHK-300 |

## Open

* Whether `@builtin` is allowed outside the prelude; today it is.
* Whether a builtin's declaration is checked against its record beyond the key; today its result type and its attributes are not.
* Whether a pattern may bind a name, which is the pattern language of [patterns](../incubator/patterns.md) and the thing that would make exhaustiveness a real analysis.
* Whether an enum reaches `int` through a cast, and what an `int` that names no case then is ([enum futures](../incubator/enum-futures.md)).
* Where a leading dot is resolved beyond a `case` scrutinee: a parameter, a field and a return type each expect a type too.
* Whether the bindings of a flat entry point are the ones its list names or the ones its body reads.
* How a splatted value reads in the emitted text once a target can take the vector whole.
