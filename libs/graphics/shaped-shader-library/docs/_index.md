# shaped-shader-library docs

Documentation hub for shaped-shader-library.
For the library overview start at the [readme](../readme.md); for the API at a glance, the [cheat-sheet](../cheat-sheet.md).

**Read [shaped-graphics' shaders.md](../../shaped-graphics/docs/shaders.md) first** for how the shader system fits together as a whole.
It is the front door — sg's vocabulary, this library's packages and reload, and the compilers below; these are the details behind it.

## Topics

- [structure](structure.md) — the roadmap with `[done]`/`[in progress]`/`[planned]` status, and the source-tree layout.
  The living design document; update it as the API lands.
- [coding-guidelines](coding-guidelines.md) — the rules this library rests on that the code cannot enforce itself.
  Short, and worth reading before changing anything here.

## Conventions

- Namespace `slib`; depends on shaped-graphics (`sg::compiled_shader`, `sg::shader_stage`, `sg::context`), and optionally shaped-shader-compiler-dxc for the HLSL→DXIL compiler.
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md).
  shaped-graphics' [editorial rules](../../shaped-graphics/docs/coding-guidelines.md) apply here too — no meta-commentary in API comments, and never contrast with past behavior.
