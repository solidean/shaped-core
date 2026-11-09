---
created: 2026-11-09
refs:
  - test: "sgl signature - DXIL's packer places every element of the repo's SGL as DXC does"
closed: 2026-11-09
resolution: fixed
---

# Lowering a shader-rosetta shader builds a vector from a u32 where its scalar belongs

What is wrong or wanted, and what is already known.

The signature golden sweeps every `.sgl` under `libs/` and `examples/`, the shader-rosetta companion's included where it is cloned, which CI's is not.
One of the companion's bgfx shaders fails to lower: `the program after lowering of 'main': b0 %190 construct: takes u32 where a part of the vector's scalar belongs`.
Seen on 2026-11-09 at `ecd4191e9`; which shader is not narrowed down yet, and the test reports no name with it.
Before `nx::source_path`'s root was normalized the sweep stopped earlier, at a path past Windows' limit (debt-cc-file-open-long-paths), which hid this.

Found on 2026-11-09: `cs_fsr_rcas_16.sgl`'s `unpack_half2x16(con.y)`.
The math lowering splatted every scalar argument up to a vector result, which is right for `pow(v, 2.0)`, refract excepted.
It is wrong for an unpack, whose word is one scalar whatever it unpacks to.
The unpacks join it, and SPIR-V lowers them now; the DXIL writer declines them as unsupported, so `prefer` takes the text route there.

Past it the sweep met a second native bug, in `fs_hextile.sgl`: an entry point returning from every branch of a nested `if` crashed the simplifier.
The outer merge is unreached and has the inner merge, unreached too, as its predecessor: a block with predecessors and no dominator, whose `-1` was used as an index.
The simplifier now steps over it.
Both have a test in `tests/ir/debug-ir-test.cc`, and the sweep passes.
