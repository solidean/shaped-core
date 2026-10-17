---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/impl/oidn_network.cc
  - file: libs/graphics/shaped-rendering/shaders/nn_conv.sgl
  - symbol: sr::oidn_denoise_routine
  - symbol: sg::feature::shader_f16
  - file: libs/graphics/shaped-rendering/docs/reconstruction.md
---

# Half precision for the OIDN network

Half precision is the one acceleration the network could take that is portable.
DX12 has it as SM 6.2 with `-enable-16bit-types`, Vulkan as `VK_KHR_shader_float16_int8` with `VK_KHR_16bit_storage`, Metal has `half` outright, and WebGPU has the optional `shader-f16` feature.
Optional on all four rather than guaranteed, so it wants a feature level rather than an assumption — but a shipped one, unlike the matrix instructions this network would really like.

What exists:
- The gate: `sg::feature::shader_f16`, which every backend answers.
- The flag: slib's SGL edge adds `-enable-16bit-types` through `shader_source_description::dxc_args`, and the network is SGL, so it takes `half` as it stands.
- The weights already arrive as fp16, and `tg::f16` widens them on load.

What is missing:
- The C++ mirror problem in [binding-preprocessor.md](../../../shaped-shader-library/docs/binding-preprocessor.md) is about constant blocks, which this network does not use.

**Expect memory rather than speed.**
The convolution is bound by cache lines rather than bytes.
That is why reading four channels at once bought 1.4x and not 4x, and an 8-byte `half4` costs the same line as a 16-byte `float4`.
What the memory buys is a larger tile, and tile size is the lever that still pays.
