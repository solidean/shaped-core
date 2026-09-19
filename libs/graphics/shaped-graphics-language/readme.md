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
    * CHECK/REQUIRE for testing

This will also later form the basis of our shader node editing

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
* The grammar mirrors the block tree of [docs/spec/syntax.md](docs/spec/syntax.md): a block that is only a `//` comment, or that ends in an open `"`, owns every deeper-indented line below it, and nothing escapes its block.
  A comment trailing code ends with its line.
* Keywords come from [docs/spec/keywords.md](docs/spec/keywords.md) — a keyword added there needs adding to the grammar too.
