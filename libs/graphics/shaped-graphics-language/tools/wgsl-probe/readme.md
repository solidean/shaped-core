# wgsl-probe

Puts WGSL through every WebGPU implementation on this machine, and says which of them accepts it.

```bash
uv run libs/graphics/shaped-graphics-language/tools/wgsl-probe/run.py shader.sgl --entry main_cs
uv run libs/graphics/shaped-graphics-language/tools/wgsl-probe/run.py some.wgsl other.wgsl
```

An `.sgl` file is compiled through the `sgl` tool first, so a run says something about the compiler rather than about a file somebody saved.

## Why two runtimes

node's WebGPU binding is **Dawn**, which Chrome ships, and deno carries **wgpu**, which Firefox ships.
Running both is how SGL's output gets checked against both browser engines with no browser involved.
It is the same pair [building-and-testing](../../../../../docs/guides/building-and-testing.md#which-runtime-executes-the-artifact) runs the wasm suites under.

Two implementations agreeing is much better evidence than one, because they word their errors differently and share no code.
Whether a storage buffer may be write-only, say, is answered by both.
Dawn says *"access mode 'write' is not valid for the 'storage' address space"*, and wgpu says *"Storage address space doesn't support write-only access"*.

node needs the `webgpu` package, which shaped-core installs under `tools/dev/js`; the script finds it there.
A runtime that is absent is skipped, and exit 2 means neither could be driven, which is neither answer.

## Both halves of the validation

The probe creates a shader module **and** a compute pipeline, because they enforce different things.
A storage texture's format rules live on the bind group layout rather than in WGSL, so
`texture_storage_2d<rgba8unorm, read_write>` passes `createShaderModule` and fails only at the pipeline —
a module-only probe reports the opposite of the truth for it.

`createComputePipelineAsync` **rejects** rather than raising a device error, so the rejection is what the probe reads.
A probe that only popped an error scope would report a clean pass for everything, which is what the first version of this one did.
