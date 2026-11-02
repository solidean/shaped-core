# Frozen shaders

A frozen build reads no shader file and hot-reloads nothing, and it carries the bytecode its common cases need.
It is what a program is shipped as: `uv run dev.py install shaped-view` installs one.

```text
SC_FROZEN_SHADERS=ON        no source directory mounted, no reload, sources from the embedded copy alone
SC_SHADER_RECORD=<file>     a run compiles every shader, serving none from a bundle, and writes each into one
sc_embed_shader_bundle      an executable carries the bundle; a request it holds compiles nothing
```

## What freezing changes

- **No disk.** `add_package` mounts each package's embedded copy and nothing over it, and the same holds for SGL module directories.
  So a frozen binary behaves the same on a machine with the source tree as on one without.
- **No reload.** `start_hot_reload` returns at once, since nothing it could watch is mounted.
- **Precompiled where recorded.** An executable that calls `sc_embed_shader_bundle(TARGET <exe>)` embeds `<build>/shader-bundles/<exe>.slibbundle`.
  A request the bundle holds is served as it was recorded, and compiles nothing.
- **Compiled where not.** Anything else compiles at run time from the embedded sources, exactly as an unfrozen build does, and logs one info line naming it.
  So DXC still ships beside a frozen binary.

A missing bundle is an empty one, so a frozen build never depends on a recording having run.
The switch is whole-build, set by the `frozen-*` presets, because it changes what slib reads rather than any type.

## The bundle key

A bundle files each shader under a hash of everything that decides it, taken in `shader_library::_compile_text` before any compiler runs.
That covers the language, format, stage, entry point, options and the namespace of an SGL file's generated types, plus the source text.
It also holds `sgl::compiler_fingerprint()` and slib's own source fingerprint, so an edit to SGL or slib misses.
It holds nothing of DXC or `ssc::dxc`, so a bundle is only as current as the recording it came from — which is why a recording never serves from one.

- **An SGL source is keyed before SGL's front end**, over its text and every module file it may `use`.
  A hit therefore skips the front end and DXC both.
- **Any other language is keyed once flattened**, after its includes are inlined and its bindings rewritten.
  What an include says is only known then, so an HLSL hit still pays its preprocess.
- **Text is hashed without carriage returns, and modules by path.**
  A recording build may read a CRLF checkout and list a directory in its own order, while a frozen one reads the embedded copy.

A shader generated at run time — an sv material permutation, a hit group — goes through the same door as a package's, so a recording catches both.

## Recording

`SC_SHADER_RECORD=<file>` makes a run write every shader it obtains, flushed record by record.
Such a run serves nothing from an embedded bundle and compiles every shader, so a frozen build can record itself.
Its recording is then what this build compiles, never an entry carried forward from an older one under a key that did not change.

The format is a header (`slibbndl`, then a version) and records of key, size and `sg::impl::encode_compiled_shader` bytes, all little-endian.
A record that does not decode is a miss, since the codec refuses whatever it doubts.
A bundle cut short serves the records before the cut and logs one error.

## Coverage

A bundle holds what its recording run reached, and nothing guarantees that is everything a user will open.
That is deliberate: the common cases are precompiled, and a rare permutation costs a compile rather than a failure.

shaped-view's run opens `tools/shaped-view/corpus/` — one small file per common case — so a new case worth precompiling is a new corpus file.
A render mode the run never draws is not recorded either, so a program that offers one has its recording draw it too.
Enumerating a permutation space explicitly, rather than reaching it by example, is open.

## The install flow

`dev.py install shaped-view` builds `frozen-clang`, runs that binary headless over the corpus with `SC_SHADER_RECORD` pointing at its build's bundle, and builds again.
The second build only re-embeds the bundle and relinks.
A failed recording warns and keeps the last good bundle, which leaves a copy that compiles more at run time and is otherwise fine.
