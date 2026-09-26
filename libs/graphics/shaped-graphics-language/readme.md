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

## Where to look

* **To write SGL**: [docs/spec/](docs/spec/_index.md) is the language, and [sgl-cube](../../../examples/graphics/sgl-cube/shaders/cube.sgl) is a shader that draws.
* **To work on the compiler**: [docs/architecture.md](docs/architecture.md) is the map of the pipeline, and [cheat-sheet.md](cheat-sheet.md) the API.
  [docs/TODO.md](docs/TODO.md) lists the short-term follow-ups, and the [spec incubator](docs/spec/incubator/_index.md) the far-off ideas.

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
```

SGL's builtins live in a C++ registry, and `prelude/builtins.sgl` is generated from it and committed.
[docs/adding-a-builtin.md](docs/adding-a-builtin.md) is how one is added.

## Editor support (VS Code)

[tools/vscode-extension/](tools/vscode-extension/) is a VS Code extension for `.sgl` files.
Today it is declarative only — a TextMate grammar plus a language configuration — so there is nothing to build and no `npm install`.
LSP support will grow in the same folder later.

### Install

VS Code loads every folder in its user extensions directory, so installing is linking this folder into it.
A link rather than a copy means a `git pull` updates the extension.

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
