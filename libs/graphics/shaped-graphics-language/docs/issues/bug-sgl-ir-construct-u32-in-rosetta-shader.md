---
created: 2026-11-09
refs:
  - test: "sgl signature - DXIL's packer places every element of the repo's SGL as DXC does"
---

# Lowering a shader-rosetta shader builds a vector from a u32 where its scalar belongs

What is wrong or wanted, and what is already known.

The signature golden sweeps every `.sgl` under `libs/` and `examples/`, the shader-rosetta companion's included where it is cloned, which CI's is not.
One of the companion's bgfx shaders fails to lower: `the program after lowering of 'main': b0 %190 construct: takes u32 where a part of the vector's scalar belongs`.
Seen on 2026-11-09 at `ecd4191e9`; which shader is not narrowed down yet, and the test reports no name with it.
Before `nx::source_path`'s root was normalized the sweep stopped earlier, at a path past Windows' limit (debt-cc-file-open-long-paths), which hid this.
