# shaped-graphics-language TODO

Running list of short-term follow-ups: what we already know we most likely want, and have not built.
An idea that may be far off, or that we may never want, belongs in the [spec incubator](spec/incubator/_index.md) instead.
What the compiler carries today is the [spec](spec/_index.md); a construct it does not carry yet is `unsupported-yet` there.
`DEBUG_` in a name marks a stand-in for something listed here.

- **Check the prelude once per process.** Every compile checks all of the prelude, bodies included, and the prelude is most of what a small program costs.
  On `debug-linux-clang` a ten-line compute shader checks in about 50 ms, nearly all of it prelude.
  The debug CI leg's SGL suite runs close to its 60-second limit for that reason, since several tests compile hundreds of programs.
  The way out is a checked prelude shared by every compile, which the checker's single arena per module does not allow yet.
  The test `sgl profile - checking a small program behind the prelude, sampled` measures it.

- **A debug build overflows its stack well inside `k_max_depth`.** A compute entry point whose store sums a 150-term chain (`x + 1.0 + … + 1.0`) crashes `sgl describe` on `debug-nopch-clang`.
  It did so before the footprint became a linear pass too.
  The recursive passes each guard their own depth at 200, which a release frame fits and a debug frame does not.
  Either the limit shrinks to what the smallest stack carries, or the deepest walks stop recursing on an operand chain.
- **Array equality and `const` arrays.** `==` of two arrays, and a `const` whose value is an array literal, are both `unsupported-yet`.
- **Values as type arguments.** `image_2d[.rgba8_unorm]` takes an enum case, and the checker reads exactly that argument today, as a special case of image types.
  The general feature is a type parameterized on an integer or an enum value, which math templated on a dimension wants as well, and it lets code branch on the value.
- **Scoped extensions.** An extension inside a type's block is `unsupported-yet` (CHK-237).
  It is meant to extend the type it names where that block alone sees it, as when implementing a method.
- **Literal folding.** `1 / 3` over integer literals alone is refused (CHK-313), and so is an operator over them that only a float takes (CHK-257).
  Folding literal subtrees is what [literal-types.md](spec/incubator/literal-types.md) sketches in their place.
- **Re-run [tools/msl-probe](../tools/msl-probe/readme.md) when the Metal toolchain is bumped.** MSL's generated reserved words are taken from one toolchain's headers, which grow every release.
  A name a newer one adds fails as `quad` did, naming a header the program's author never wrote, until the block is regenerated.
- **What `discard` does to a quad's derivatives, per target.** SGL writes `discard;` and MSL `discard_fragment();`, which every target reads as "no effect after this".
  Whether the pixel keeps running as a helper is where they differ, and a sample after a discard in a neighbouring pixel depends on it.
  sg's tier-1 pixel-semantics test pins it on every backend, and dx12 and vulkan keep the pixel as a helper.
  A target that terminates gets the emulation the design settled: a flag, guarded stores, and the real discard at the end.
- **Compact file-sampler numbering per `pipeline` declaration.** A file-scope sampler's index is its position among all the file's samplers (EMIT-133).
  So a file past 16 is `too-many-samplers` even where each stage reaches few.
  The way out not taken yet: a `pipeline` numbers only the samplers its stages reach, and emits those stages with its numbers.
  It costs an entry point's text depending on its pipeline, and a stage shared by two pipelines compiling twice.
  A hand-assembled pipeline keeps declaration order.
- **Textures, images and samplers in a test's bindings.** A test lists values, buffers and acceleration structures, and its driver binds them (CHK-333, EVAL-94).
  A binding that holds a texture, an image or a sampler is `unsupported-yet` in a test, since the interpreter has no texel to read yet.
  It needs a texture a driver can bind, `member_data` taking its texels, and the texture builtins' evaluators reading them.
- **A watertight triangle test in the emulated trace.** Möller–Trumbore misses or doubles a ray through a shared edge ([raytracing-polyfill.md](raytracing-polyfill.md#traversal)).
- **A spatial sort for the webgpu BVH.** sg builds the tree over the primitive order, so a triangle soup traces slowly ([raytracing-polyfill.md](raytracing-polyfill.md#building)).
  A proper build sorts primitives along a space-filling curve on the GPU first, which wants a GPU sort sg does not have yet.
- **Compacting sg's acceleration pool.** A freed region is reused first-fit, and the pool only grows, so a scene that rebuilds often fragments it.
- **`prelude/raytracing.sgl` is ~1200 lines, and its three emulated traversals repeat one stack walk.** Triangles, boxes and both differ only in what a BLAS leaf does.
  One walk taking the leaf as a function value (CHK-317) would carry all three, once the inliner's output of it is as tight as the copies are.
- **Metal's pipeline falls short of DXR in three places** (the spec's raytracing file, "Per target"), none tested on hardware here.
  A hit's instance transforms are the identity, since Metal hands them only under intersection tags sg's tables do not declare yet.
  `accept_and_end_search` acts as `accept` in an intersection function, which can only accept or not.
  A dispatch traces one TLAS: the closest hit's record comes from one buffer of instance offsets, which sg binds at buffer 5.
- **A host's groups make a payload state the widest access.** A payload a `.host` pipeline traces is `read`/`write` for every stage on HLSL (EMIT-137).
  A group compiled apart cannot agree with the module's inferred qualifiers otherwise.
  The way out is compiling the host's groups against the module's inferred qualifiers, which slib would hand to `compile_hit_group`.
  hlsl.cc writes `#pragma dxc diagnostic ignored "-Wpayload-access-perf"` into every ray-tracing stage, where only a stage of a widest payload needs it.
- **Shared source for a host's hit groups.** `slib::compile_hit_group` compiles a group from SGL text the host concatenates with the ray set's declarations.
  That is a stopgap for `use`: the group's file would import the module that declares the set.
- **A generic struct's type parameter is not in scope in its methods.** `mixed_hit.procedural()` returns through the helper `procedural_of_mixed[A]`, since the method cannot name `A`.
- **An enum does not convert to `int`.** `h.kind as int` is refused (CHK-150), so storing a `hit_kind` in a buffer takes a `case`; [enum-futures.md](spec/incubator/enum-futures.md) holds the cast.
- **The matrix zoo, and `tg` types such as `quat`.** A hit's transforms are rows of `float4` because SGL has `mat4` alone; `mat3x4`, `mat3` and a quaternion would let a hit hand them over typed.
- **A `misplaced-not` inside a `test` body was seen to pass silently**, while writing the ray-tracing tests.
  It does not reproduce in a plain test body: `test:` ending in `not a and b` reports it, as a function body does.
  The likelier case is a test with an `error` or `warning` expectation, which takes every diagnostic inside it as its own (CHK-232); pin whichever it was with a corpus test.
- **What is left of texture methods** is [texture-methods.md](spec/incubator/texture-methods.md)'s: subscripts, and gathers of integer textures.
- **A `require` inside a nested block, and `if feature f:` to branch on one.** A builtin's call counts its feature where the entry point reaches it (CHK-322), so a body uses features now.
  What is missing is scoping a grant to a block, and a branch that leaves a use out on a device without the feature.
- **Features used through another symbol.** A binding's `required` counts only the uses resolved while its members compile, and `checker::compile` clears the grant around any symbol they demand.
  No such symbol can hold a resource yet; once a type alias or a struct field can, its use has to reach every binding that names it.
- **Features across a hot reload outside a `pipeline`.** A declared pipeline freezes its features, so a reload needing another one keeps what it had.
  A compute shader or a stage acquired on its own has no frozen part: its reload compiles, and its pipeline is then refused by the feature's name.
- **Unsigned literals by suffix.** A literal takes a `uint` wherever one is expected (CHK-253), and `1u` is `unsupported-yet` (CHK-61).
  Whether the suffix is needed at all is the question [literal-types.md](spec/incubator/literal-types.md) holds.
- **Arrays and `mat3` in GPU memory, and matrices and arrays across a stage edge.** An array is a value everywhere else (CHK-285).
  It is `unsupported-yet` wherever GPU memory or a stage edge holds one (CHK-291).
  `mat3` does not exist yet.
  Their layout is decided (the spec's emitting file, "Open"), and they land together, after the tier-1 work.
  In a constant block an array element starts a row: `slib::row<T>` pads a shorter one to 16 bytes on the host, and WGSL reads it through `array<vec4f, N>`.
  The mechanism that is missing is a memory form holding an array, which a runtime index walks into.
  A `mat3` is its three columns, each starting a row, in a block (44 bytes), and `slib::gpu_mat3` holds that on the host; in a buffer's element it is 36 bytes, which is `tg::mat3f` itself.
  WGSL passes no matrix and no array between stages, so both cross as one location per column or element.
- **The layout double check on dxil.** slib compares what SPIR-V and WGSL place against what SGL states, and reads no DXIL layout: the bytecode carries no reflection container to read it from.
  dx12's packing is SGL's own rule, so this is the target least likely to disagree, and a DXIL arm would need the container kept beside the bytecode.
- **A linter for SGL's own style, starting with `@expect` on its own line.** An `@expect(…)` stands on the line above its `test`, never before it on the same line; every file here follows that.
  It is a rule of `@expect` and not of attributes: `@vertex fun main(…)` on one line reads fine and stays.
  The parser takes both spellings, so only a linter can hold the line.
- **One test that the highlighters agree with the compiler.** The VS Code grammar and the review tool's lexer each copy the syntax, and only the lexer's keyword set is checked today.
  The test tokenizes a corpus, `tools/vscode-extension/examples/sample.sgl` at least, with the compiler, the grammar and the lexer.
  It compares the class each assigns to every token: keyword, name, number, string, comment, operator.
  The compiler's side is `sgl::classify` without a checked module: the syntactic classes, plus a class for each name the file declares.
  It runs the TextMate grammar through `vscode-textmate`, or through a Python TextMate engine if one is good enough to spare the Node dependency.
- **`no-effect` in a test, by what can reach an assert.** A line of a test without an effect is still code under test when it can reach an `assert`, since it may trigger one.
  So CHK-225's warning is too coarse there, and a `void` line is exempt from it today.
  The precise rule wants the check pass to know more about the effects of an expression and of a function, "could reach an assert" among them.
  A line of a test is then `no-effect` exactly when it has no effect and cannot reach an assert.
