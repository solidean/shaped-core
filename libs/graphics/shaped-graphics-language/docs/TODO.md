# shaped-graphics-language TODO

Running list of short-term follow-ups: what we already know we most likely want, and have not built.
An idea that may be far off, or that we may never want, belongs in the [spec incubator](spec/incubator/_index.md) instead.
What the compiler carries today is the [spec](spec/_index.md); a construct it does not carry yet is `unsupported-yet` there.
`DEBUG_` in a name marks a stand-in for something listed here.

- **Hex literals.** `0xFF` is a number, and the checker refuses it as `unsupported-yet: a hex literal`.
  `classify_number` puts every prefixed literal in `number_class::other`, since those need literal types the checker does not have.
  A stencil mask in a `pipeline` is where it bites first.
- **Binary literals.** `0b1010`, the same way and for the same reason: `unsupported-yet: a binary literal`.
- **Values as type arguments.** `image_2d[.rgba8_unorm]` takes an enum case, and the checker reads exactly that argument today, as a special case of image types.
  The general feature is a type parameterized on an integer or an enum value, which math templated on a dimension wants as well, and it lets code branch on the value.
- **Scoped extensions.** An extension inside a type's block is `unsupported-yet` (CHK-237).
  It is meant to extend the type it names where that block alone sees it, as when implementing a method.
- **Literal folding.** `7 / 2` is refused while no `/` takes `int` (CHK-257); folding literal subtrees is what [literal-types.md](spec/incubator/literal-types.md) sketches in its place.
- **Texture methods past 2D.** `sample`, `load`, `store` and `size` cover 2D shapes, with the sampler always passed.
  A default sampler per texture, the other shapes and subscripts are [texture-methods.md](spec/incubator/texture-methods.md).
- **Features used in a body.** Only a binding member uses a feature today, so a body's `require` can only declare one for its entry point (CHK-262).
  The first builtin or binding array that needs one in a body brings the use into the inlined entry point, and `feature-not-declared` then names the call chain down to it.
  A `require` inside a nested block, and `if feature f:` to branch on one, wait for that too.
- **Features across a hot reload outside a `pipeline`.** A declared pipeline freezes its features, so a reload needing another one keeps what it had.
  A compute shader or a stage acquired on its own has no frozen part: its reload compiles, and its pipeline is then refused by the feature's name.
- **Unsigned literals by suffix.** A literal takes a `uint` wherever one is expected (CHK-253), and `1u` is `unsupported-yet` (CHK-61).
  Whether the suffix is needed at all is the question [literal-types.md](spec/incubator/literal-types.md) holds.
- **File-scope samplers.** A `sampler name:` at file scope is a pipeline layout's `sg::bound_sampler`, which vulkan and metal do not bind yet (sg's TODO.md).
  Once they do, it needs an HLSL address outside every group's space, and the generated `acquire_pipeline` to fill `static_samplers`.
  It will join the pipeline layout of every entry point that uses it, transitively; until it lands, it is `unsupported-yet`, never refused as invalid.
- **One test that the highlighters agree with the compiler.** The VS Code grammar and the review tool's lexer each copy the syntax, and only the lexer's keyword set is checked today.
  The test tokenizes a corpus, `tools/vscode-extension/examples/sample.sgl` at least, with the compiler, the grammar and the lexer.
  It compares the class each assigns to every token: keyword, name, number, string, comment, operator.
  It runs the TextMate grammar through `vscode-textmate`, or through a Python TextMate engine if one is good enough to spare the Node dependency.
- **`no-effect` in a test, by what can reach an assert.** A line of a test without an effect is still code under test when it can reach an `assert`, since it may trigger one.
  So CHK-225's warning is too coarse there, and a `void` line is exempt from it today.
  The precise rule wants the check pass to know more about the effects of an expression and of a function, "could reach an assert" among them.
  A line of a test is then `no-effect` exactly when it has no effect and cannot reach an assert.
