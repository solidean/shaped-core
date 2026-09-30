# msl-probe

Finds every name MSL takes from a program, and prints the generated block of MSL's reserved words.

```bash
uv run libs/graphics/shaped-graphics-language/tools/msl-probe/run.py
```

The output replaces the block of `k_msl` in [reserved_words.cc](../../src/shaped-graphics-language/emit/reserved_words.cc) that starts with "Everything else the Metal toolchain declares", as it is.
**Re-run it when the Metal toolchain is bumped**, since Metal's headers grow every release.

## What it probes

SGL's MSL says `using namespace metal;`, so every name of the standard library stands where the program's structs are declared.
Metal also declares names at global scope of its own, such as the `quad` it reserves for a type it never defines, and its headers define macros.
A struct or a function of the program that shares such a name fails to compile, naming a header its author never wrote.

So each identifier of the installed toolchain's headers is declared once as a struct and once as a function, and a name either declaration fails for is taken.
The probe's own names carry an `sgl_probe_` prefix, so a candidate never collides with the probe rather than with Metal.
A name that fails in the parallel pass is compiled again alone before it counts, since a compiler starved of resources fails now and then.

The names `k_msl` already holds above the block, its keywords and hand-picked types, are left out.
Over-reserving costs an underscore and nothing else, so a newer toolchain can only add names.

It needs Apple's Metal toolchain, which Xcode installs as a separate component (`xcodebuild -downloadComponent MetalToolchain`); exit 2 means no `metal` was found.
A run takes about a minute.
