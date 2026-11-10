# lod-probe

Which mip level a GPU's sampler picks with anisotropic filtering on, through D3D12 and through Vulkan, on every GPU of this machine.

On NVIDIA GPUs the two APIs disagree by up to a quarter of a mip level, with the same sampler on the same GPU.
We would like to know where else that holds: other GPUs, other drivers, other vendors.

## Running it

Windows 10 or 11, 64-bit, with a D3D12 and a Vulkan driver installed (any current GPU driver has both).

1. Unzip the folder anywhere.
2. Open a terminal in it, and run `lod-probe.exe`.
   It takes a few seconds per GPU, and prints its tables as it goes.
3. Send us the `lod-probe-<gpu>.txt` and `lod-probe-<gpu>.json` it writes into that folder, one pair per GPU.

`lod-probe.exe --images` also writes pictures of the difference, a few MB per GPU.
`lod-probe.exe --list` only lists the GPUs it found.
`lod-probe.exe --adapter 1` probes only the GPU listed as `[1]`.
`lod-probe.exe --out <folder>/<name>` writes elsewhere; the folder must exist.

Please mention whether you changed any texture-filtering setting in your driver's control panel, since those may change the result.

## Reading the result

Each table has a column per anisotropy (`x1` is off) and a row per way of choosing a level.

- `hardware LOD minus specified LOD` is how far each API's level is from what the specification says; near 0 is expected.
- `blend width` is how quickly a mip blends into the next; 0.800 is a plain linear blend, and smaller means the driver sharpens it.
- `dx12 minus vulkan` is the gap itself; all zeros means the two APIs agree on that GPU.
- `dx12 minus vulkan+sg` is the same after shaped-core's correction, which should read 0 everywhere.

`largest gap` at the end of each comparison summarises it in one line.

## For shaped-core developers

`uv run dev.py run lod-probe [args]` runs it from the tree.
`uv run libs/graphics/shaped-rendering/tools/lod-probe/package.py` builds the frozen preset and packs this folder as `build/lod-probe.zip`.

What the measurement does and why is in [probe.hh](probe.hh).
What it found, and the correction sg's vulkan backend applies, is [anisotropic-lod.md](../../../shaped-graphics/docs/concepts/anisotropic-lod.md).
