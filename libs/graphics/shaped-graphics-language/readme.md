# Shaped Graphics Language

Our own portable shading language with modern convenience features.

Supports:

* cross compilation to hlsl, hlsl-spirv, wgsl, msl 
* targets dx12, vulkan, metal, webgpu
* self contained tooling for shader live editing
* tight model for integration with `sg`
* polyfills for features like ray-tracing in webgpu
* tooling, syntax highlighting, LSP, etc.
* composability (libraries of shaders)
* advanced in-shader features:
    * per expression traces
    * assertions
    * logging
    * `test` declarations, run on the interpreter, whose bool lines are checks

This will also later form the basis of our shader node editing

## Performance

**The aspiration: SGL performs no worse than HLSL written the same way, for everyday shader code.**
A reader should be able to make the same performance deductions from SGL as from HLSL: where a loop runs, what a branch costs, what is loaded and how often.
The emitted text is not written to be read, but it is written for the target's compiler to turn into what the hand-written HLSL would have become.
Inlining, scalar replacement and constant folding are the target compiler's.
SGL's job is to leave it nothing that blocks them: no copy it cannot see through, no exit it cannot structure, no load in a loop it cannot hoist.
Where SGL falls short of that, it is a bug of the compiler and is filed under [docs/issues/](docs/issues/) as `perf-sgl-*`.
What a target lacks is the exception: WGSL's emulated ray tracing pays for being software, and that cost is the polyfill's, not SGL's.

`uv run dev.py benchmark "sr - SGL against the HLSL it was ported from"` measures it on dx12, with sr's shaders against the hand-written HLSL they were ported from.
On an RTX 5070 Ti in October 2026 SGL took 0.99x to 1.00x the HLSL's GPU time for à-trous, 0.99x for the network's convolution, and 1.02x for SVGF's filter.
On an Intel Arc B580 in November 2026 it took 0.99x to 1.00x of it on dx12, and 0.97x to 0.99x on vulkan, where both go through DXC's SPIR-V backend.
`uv run dev.py benchmark "sr - SGL against the same kernels written in MSL, on metal"` measures the same kernels against MSL ports of that HLSL.
On an Apple M4 in November 2026 SGL's minima were 0.99x to 1.05x the MSL's for à-trous, 0.97x to 1.12x for SVGF's filter, and 1.00x to 1.02x for the convolution.
`uv run dev.py benchmark "written in WGSL" --preset emscripten-webgpu-relwithdebinfo` measures WebGPU, against the same kernels written by hand in WGSL.
On the same Arc it took 1.00x for à-trous and SVGF's filter under node's Dawn and deno's wgpu alike, and 0.81x and 0.52x for the convolution.
The convolution is faster because SGL unrolls the loops the HLSL marks `[unroll]`, which WGSL cannot ask for.

## Where to look

* **To write SGL**: [docs/spec/](docs/spec/_index.md) is the language, and [sgl-cube](../../../examples/graphics/sgl-cube/shaders/cube.sgl) is a shader that draws.
* **To work on the compiler**: [docs/architecture.md](docs/architecture.md) is the map of the pipeline, and [cheat-sheet.md](cheat-sheet.md) the API.
  [docs/issues/](docs/issues/) holds what is known to be open, and the [spec incubator](docs/spec/incubator/_index.md) the far-off ideas.
* **To work on the language server**: [docs/lsp.md](docs/lsp.md) is its design, what it measures and where it grows.

## The `sgl` command line

[tools/sgl/](tools/sgl/) is the command line of the toolchain: one nexus binary whose jobs are `COMMAND`s, so the formatter, the linter and the language server join it as further commands.
It is built with the library wherever `SC_BUILD_TOOLS` is on, and run through `dev.py`, which builds it first:

```bash
uv run dev.py run sgl                                   # what the binary holds
uv run dev.py run sgl -- emit shader.sgl --entry main_ps --target wgsl
                                                        # the target text, or the diagnostics; hlsl-dx12, hlsl-vulkan, wgsl, msl
                                                        # --run-tests runs the file's tests first, and writes nothing if one fails
uv run dev.py run sgl -- test a.sgl b.sgl               # checks the files and runs their tests: what failed, and why
uv run dev.py run sgl -- prelude                        # prelude/builtins.sgl as the builtin registry generates it
uv run dev.py run sgl -- prelude --check <path>         # exit 2, and where the texts part, when the file differs
uv run dev.py run sgl -- prelude --write <path>         # what `uv run dev.py check sgl-prelude --fix` runs
uv run dev.py run sgl -- describe shader.sgl            # what slib's package generator reads, as JSON
uv run dev.py test sgl                                  # the language server's tests, which the binary carries
```

`sgl lsp` is the language server, which an editor starts and talks to over stdin and stdout; the extension below does.

SGL's builtins live in a C++ registry, and `prelude/builtins.sgl` is generated from it and committed.
[docs/adding-a-builtin.md](docs/adding-a-builtin.md) is how one is added.

## Editor support (VS Code)

[tools/vscode-extension/](tools/vscode-extension/) is a VS Code extension for `.sgl` files.
It carries a TextMate grammar and a language configuration, and a client for the language server `sgl lsp`.
The client is committed as an esbuild bundle, `dist/extension.js`, so installing it needs no `npm install`.

### Install

Build `sgl` first, since the extension runs the binary your build made: `uv run dev.py build -t sgl`.

VS Code loads every folder in its user extensions directory, so installing is linking this folder into it.
A link rather than a copy means a `git pull` updates the extension.

`uv run dev.py install sgl-vscode` does both steps: it builds `sgl`, then links the folder into every VS Code-family editor whose extensions directory exists.
`--editor code` picks one, `--uninstall` removes this checkout's links again, and `uv run dev.py install` shows where it is installed.
It warns when a packaged (VSIX) install of the extension sits beside the link, since the editor would then load it twice.
By hand, the link is:

Windows (PowerShell, from the repo root — a junction needs no admin rights):

```powershell
New-Item -ItemType Junction `
    -Path "$env:USERPROFILE\.vscode\extensions\shapedcode.sgl" `
    -Target (Resolve-Path libs\graphics\shaped-graphics-language\tools\vscode-extension)
```

Linux / macOS (from the repo root):

```bash
ln -s "$PWD/libs/graphics/shaped-graphics-language/tools/vscode-extension" ~/.vscode/extensions/shapedcode.sgl
```

Then run **Developer: Reload Window**.
Open [examples/sample.sgl](tools/vscode-extension/examples/sample.sgl) to check: the language mode in the status bar reads "Shaped Graphics Language".

The extensions directory differs for other builds: `.vscode-insiders`, `.vscode-oss`, `.cursor`, and `.vscode-server` when working over Remote / WSL.
To uninstall, delete the link and reload.

### The formatter

`sgl format FILE…` writes SGL's spacing into each file in place; `--check` changes nothing and exits 2 where a file would change.
`dev.py check` holds every tracked `.sgl` file to it through the `sgl-format` gate, which `--fix` formats.
It is the spacing within a line: one space where tokens stand apart, a space after a comma and a colon, around an arrow and around a binary operator, none inside brackets or before a comma or a colon.
So a type is `name: type` everywhere, `let x: T = …` included.
The trailing comments of consecutive lines line up one space past the longest code, unless that moves a comment more than 16 columns, which starts a new run.
Code is indented four spaces a level of the line tree, a tab in the indentation included.
The lines a comment or a multi-line string owns move with it, so the comment keeps its layout and the string its value.
Line breaks, comment text and strings are content, and stay as written.
A syntax error leaves its block and the block's siblings as written, with a warning naming the error, and the rest of the file is formatted; at the top level, its declaration stays.
An operator without its spaces, `a+b`, and a tab in the indentation are no such errors: each has one reading, which the formatter fixes.
Its output always parses to the tree its input did, which it checks; a line whose formatting would not is left as written and named, as a bug of the formatter.

### The language server

The extension starts the server when the first `.sgl` file opens, and its log is the **SGL Language Server** output channel.
What the server provides:

* diagnostics, pushed as you type;
* semantic tokens, which colour what the grammar cannot tell apart;
* inlay hints showing inferred types: ` : float` after a `let` that writes none, and `-> float` before the `=>` of a function that writes none;
* hover: a name's declaration as SGL writes it, `fun mix(a: float, b: float, t: float) -> float`, with its `///` comment, and the rest of its overload set;
* go to definition, into the prelude too, whose files open as read-only `sgl-prelude:` documents;
* completion: after a `.` what the object has, anywhere else the locals in scope, the declarations and the keywords;
* signature help while a call's arguments are written, every overload listed and the argument under the cursor marked, a named one by its name;
* find all references and rename, across the document and its modules; a name the prelude declares is not renamed;
* formatting, by the rules of `sgl format` above;
* an outline of the document's declarations, folding by indentation and comment runs, and workspace symbols across the open documents and the module directories;
* test results: a failing `check` is a diagnostic, and every check site gets a gutter mark.

Accepting a hint writes it into the source, except on a property of a type body, which takes no `-> type`.
An open `prelude/builtins.sgl` or `prelude/core.sgl` is checked as that file of the prelude, so editing `core.sgl` is checked live.

The gutter marks are a green check when every run passed, a red cross when every run failed, and a half-and-half mark for both.
A grey question mark is a site that never ran, and hovering any mark gives the counts.
An edit dims the marks to a dashed circle until the server reports on the edited text.

A diagnostic can point into the prelude, and such a link opens a read-only `sgl-prelude:` document the server supplies.

**Which binary runs.**
The setting `sgl.server.path` wins when it is set.
Otherwise the extension reads `build/*/nexus-binaries.json` in every workspace folder, whose `paths` object configure writes.
It takes the `sgl` of the platform's default preset, the one `uv run dev.py build` builds without `--preset`.
Without one it takes the newest `sgl` of any other build that runs on this machine, skipping wasm and other platforms' builds.
The output channel names the binary it chose, and why.

**The server runs from a copy.**
The binary (with its `.pdb` on Windows) is copied into the extension's global storage first, so a rebuild can replace the original while the server runs.
When the original changes, the extension offers to restart with "A newer sgl was built."
A rebuild of the default preset's `sgl` offers it too, even while a fallback runs.
**SGL: Restart Language Server** does the same on demand: it stops the server, copies the binary again, and starts it.

**Settings.**

* `sgl.server.path` — the `sgl` binary to run; empty (the default) finds it through the manifests.
* `sgl.server.logLevel` — `trace`, `debug`, `info` (the default), `warning` or `error`, handed to the server at start.

Changing either restarts the server.

### Working on the client

* The client is [src/extension.js](tools/vscode-extension/src/extension.js), plain CommonJS over `vscode-languageclient`.
* After changing it, run `npm install && npm run bundle` in `tools/vscode-extension/` and commit `dist/extension.js` with the change.
* `uv run dev.py check` rebuilds the bundle whenever its sources changed and fails when the committed one differs; `--fix` replaces it.

A manual test after a change, with the extension installed by `uv run dev.py install sgl-vscode` and the window reloaded:

1. Open [tests/samples/control-flow.sgl](tests/samples/control-flow.sgl): names are coloured by the server, and inferred types appear as inlay hints.
   [tests/corpus/calls/lookup.sgl](tests/corpus/calls/lookup.sgl) shows the `-> float` hints of arrow functions.
2. Type a deliberate error: a squiggle appears, and its hover names the problem.
3. Open a file with tests, such as [tests/corpus/calls/defaults.sgl](tests/corpus/calls/defaults.sgl): every check has a green gutter mark.
   Break one so that it fails: its mark turns into a red cross, and the test is a diagnostic.
4. Open [prelude/builtins.sgl](prelude/builtins.sgl): it checks without a diagnostic.
5. Rebuild with `uv run dev.py build -t sgl`: the "A newer sgl was built." prompt appears, and **Restart** brings the new server up.
   The output channel's `using …` line names the default preset's build directory.
6. With the marks of step 3 showing, add a line above a check: its mark moves down with it, and stays on that line once it dims.
7. Hover a call of `mix`: its signature and its doc comment show.
   **Go to Definition** on it opens `builtins.sgl` from the prelude at its declaration.
8. In a function taking a struct `l`, type `l.`: the completion list offers its fields and methods.
9. Type `mix(` and a first argument and a comma: signature help shows `mix`'s overloads with the second parameter marked.
10. **Rename Symbol** on a local: every use changes; on `mix` it is refused.
    **Find All References** on a function lists its declaration and its calls.
11. Write `a+b` and **Format Document**: it becomes `a + b`.
12. The **Outline** view lists the file's declarations, a struct's fields under it; **Go to Symbol in Workspace** finds a function by a few of its letters.

### Working on the grammar

* The grammar is [syntaxes/sgl.tmLanguage.json](tools/vscode-extension/syntaxes/sgl.tmLanguage.json); a change shows up after **Developer: Reload Window**.
* To try a change without installing, open `tools/vscode-extension/` as the workspace folder and press F5 — that starts an Extension Development Host with it loaded.
* **Developer: Inspect Editor Tokens and Scopes** shows which scope a token received, which is how to tell a grammar bug from a theme that does not color that scope.
* The grammar mirrors the [line tree](docs/spec/syntax/line-tree.md) of the [syntax specification](docs/spec/syntax/_index.md), which is the authority.
  A line that is only a `//` or `///` comment, or that ends in an open `"`, owns every deeper-indented line below it ([strings and comments](docs/spec/syntax/strings-and-comments.md)).
  Nothing escapes its indentation.
  A comment trailing code ends with its line.
* Inside a line it follows [tokens](docs/spec/syntax/tokens.md): `-` is an operator and never part of a name, and `'` continues a symbol but never starts one.
  A number is several fused tokens there, so the grammar assembles it by the rules of [numbers](docs/spec/syntax/numbers.md).
* TextMate JSON has no variables, so the symbol character class and the keyword lookahead repeat in many patterns — change every copy.
* Keywords come from [docs/spec/keywords.md](docs/spec/keywords.md) — a keyword added there needs adding to the grammar too, in `#keywords` and in that lookahead.
* The review tool's lexer, [sgl_lexer.py](../../../tools/review/lib/render/sgl_lexer.py), is a second copy of the same rules: change it with the grammar.
  [architecture.md](docs/architecture.md#the-syntactic-half) says what is checked between them.
* [examples/sample.sgl](tools/vscode-extension/examples/sample.sgl) exercises every construct the grammar knows; extend it with the grammar.
  Its last section holds the spellings the compiler reports, so everything above it stays valid SGL.
