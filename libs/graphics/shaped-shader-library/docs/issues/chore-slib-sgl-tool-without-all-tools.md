---
created: 2026-10-20
---

# A consumer needs SC_BUILD_TOOLS=ON for any SGL package

`sc_add_shader_package` runs the `sgl` tool at build time, and the tool is built only with the tools, which also builds the linter and the language server.
A consumer that writes HLSL, WGSL or MSL alone does not need it; one writing SGL could build `sgl` alone, behind a narrower option.
