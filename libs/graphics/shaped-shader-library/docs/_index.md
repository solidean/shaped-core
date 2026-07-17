# shaped-shader-library docs

Documentation hub for shaped-shader-library. For the library overview start at the
[readme](../readme.md); for the API at a glance, the [cheat-sheet](../cheat-sheet.md).

**For how the shader system fits together as a whole — sg's vocabulary, this library's packages and
reload, and the compilers below — read [shaped-graphics' shaders.md](../../shaped-graphics/docs/shaders.md)
first.** It is the front door; these are the details behind it.

## Source organization

```text
shaped-shader-library/
  fwd.hh              # fwd decls + handle typedefs
  all.hh              # umbrella
  shader_package.hh   # what the generator emits: definitions + embedded files + source dir
  shader_asset.hh     # one shader: acquire(ctx), generation(), the pending/current slots
  shader_library.hh   # mounts, compilers, the compile path, the reload watcher
  filesystem/         # the mountable VFS: the interface, mount_table, memory/embedded/real
  compiler/           # the shader_compiler seam + the concrete compilers (dxc)
  impl/               # internal: the reload watcher
cmake/                # sc_add_shader_package + the package generator
```

## Topics

- [structure](structure.md) — the roadmap with `[done]`/`[in progress]`/`[planned]` status. The living
  design document; update it as the API lands.
- [coding-guidelines](coding-guidelines.md) — the rules this library rests on that the code cannot
  enforce itself. Short, and worth reading before changing anything here.

## Conventions

- Namespace `slib`; depends on shaped-graphics (`sg::compiled_shader`, `sg::shader_stage`,
  `sg::context`), and optionally shaped-shader-compiler-dxc for the HLSL→DXIL compiler.
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md). shaped-graphics'
  [editorial rules](../../shaped-graphics/docs/coding-guidelines.md) apply here too — no
  meta-commentary in API comments, and never contrast with past behavior.
