# Where tests go next

*Incubator: not normative.*

## The idea

`test` declarations exist ([CHK-224](../semantics/checking.md#tests)), and they run on the interpreter.
Six directions were agreed and left for later, and each builds on what is there without changing it.

**Running tests on the GPU.**
A test has no stage and no parameter, and binds only what it lists, so it can be written as a compute shader of one thread.
Each check writes one bit into a buffer the toolchain reserves, the one [shader-logging.md](shader-logging.md) plans for `assert`.
The same test then runs on the interpreter and on each backend, and a result that differs is a bug of the compiler or of a driver.
That is the differential test that already compares the two forms of a flat tree, reaching the real targets.
It needs `void` to be written everywhere, which it is, and a check to stay its own statement rather than a bare `bool`, which it does.

**Resource values in tests.**
A test lists its bindings, and a C++ driver gives their values, buffers and acceleration structures included (CHK-333, EVAL-94).
A test states a texture's texels and a dynamic sampler's state itself (EVAL-127, EVAL-128), so a test of a sampling function needs no driver.
A buffer's contents and a value member still come from the driver alone, and `sgl test` sees them as zero.
They want something like a resource literal that exists only on the interpreter, `buffer[float](1.0, 2.0)`, and the local binding of [binding-effects.md](binding-effects.md) is where it plugs in.

**Capturing what is constant after inlining.**
A test reads only a `const` of the function it stands in.
A local such as `let k = 0.5` is constant once the function is inlined, and [inferred-comptime.md](inferred-comptime.md) is the analysis that would let a test read it.

**Tests inside generic functions and types.**
A test inside a generic reads its generic arguments, so it runs once per instantiation, after one exists.
The keyword is seen before any instantiation, so the test is registered at once, and a module whose generic is never instantiated fails its tests with "this test was not run".
That keeps a test fail-closed where it cannot run yet.
The way out is a module-level test that instantiates the generic and ends in `true // run the generic tests`.

**A simulated workgroup.**
A test that reaches a subgroup operation or a derivative runs as the lanes of a subgroup, one after another (EVAL-140 to EVAL-145).
A barrier still waits for nobody, and workgroup memory is each lane's own (EVAL-92).
A test could name a workgroup size instead, and the interpreter run that many invocations, switching between them at each barrier and interleaving atomics in a fixed order.
Races and barrier placement would then be testable off the GPU, deterministically; it needs a scheduler in the interpreter and a spelling for "run this test as a workgroup of 64".
The lanes' exchange is already that scheduler's easy half: what crosses a lane is settled by running the lanes again until nothing they give changes.

**Matching an expected diagnostic by its message.**
`@expect(error = "kind")` matches the kind's name only, which is stable where a message is not.
A second argument that matches the message too is additive, and is worth building only once a test needs to tell two diagnostics of one kind apart.

## What it touches

* The emitters and `sg`, for tests on the GPU: a compute entry point per test, and a buffer the host reads back.
* The interpreter's lanes, for a workgroup that waits at its barriers.
* The check pass, for captured constants and for message matching.

## Already fixed by the syntax

* `test` is a declaration keyword, and `@expect` an attribute of a test, so every direction above is new arguments or new analysis rather than new syntax.

## Open

* How a GPU run reports a failing check's values, rather than only that it failed.
* How a resource literal is spelled, and whether it may stand outside a test.
