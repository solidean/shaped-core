---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/tools/vscode-extension/syntaxes/sgl.tmLanguage.json
  - file: tools/review/lib/render/sgl_lexer.py
  - file: libs/graphics/shaped-graphics-language/tools/vscode-extension/examples/sample.sgl
  - symbol: sgl::classify
---

# One test that the highlighters agree with the compiler

The VS Code grammar and the review tool's lexer each copy the syntax.
Only the lexer's keyword set and both copies' subgroup and quad names are checked today.

The test tokenizes a corpus, `tools/vscode-extension/examples/sample.sgl` at least, with the compiler, the grammar and the lexer.
It compares the class each assigns to every token: keyword, name, number, string, comment, operator.
The compiler's side is `sgl::classify` without a checked module: the syntactic classes, plus a class for each name the file declares.
It runs the TextMate grammar through `vscode-textmate`, or through a Python TextMate engine if one is good enough to spare the Node dependency.
