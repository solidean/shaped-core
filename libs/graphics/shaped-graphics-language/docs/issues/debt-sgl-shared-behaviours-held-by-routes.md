---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_sync.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/lower_call.cc
---

# Three behaviours every route shares are still written once per route

What is wrong or wanted, and what is already known.

LEGAL-70 and LEGAL-71 moved `print` and the workgroup prologue above the routes, so neither route knows what they are.
Moving the prologue found three more behaviours with the shape those two had, each written in the text route and again in the native lowering.

| behaviour | text route | native route | what moving it needs |
|---|---|---|---|
| EMIT-250, a float atomic with no device add | about 150 lines, `register_sync.cc` 514 to 661 | about 40 lines, `lower_call.cc` and `lower_memory.cc` | the device's float-atomic support as a legalize option |
| EMIT-251, `bool32` conversions | about 30 lines, `register_conversions.cc` | a few cases, `lower_call.cc` and `lower_memory.cc` | nothing known |
| inclusive scans and shuffle by offset | HLSL helpers, `register_subgroups.cc` | the DXIL writer, `writer.cc` and `writer_resources.cc` | only dx's two routes share it, so a legalize step would serve HLSL and DXIL alone |

Line counts are from 2026-11-07 and were read, not measured.
The target state is LEGAL-71's: legalize writes the behaviour as ordinary flat statements, and each route lowers those.
Whether the flat tree can already say each one has not been checked.
