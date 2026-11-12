---
created: 2026-11-12
priority: next
refs:
  - test: "sg - the first-meeting check of an array declaration*"
  - file: libs/graphics/shaped-graphics/tests/barrier/sticky-array-test.cc
---

# Dropping a dx12 list after an asserted dispatch crashes in ~command_list

'sg - the first-meeting check of an array declaration compiles out with the other asserts' (tests/barrier/sticky-array-test.cc) crashes every run on an RTX 4090, dx12 hardware backend, relwithdebinfo-clang.
Its CHECK_ASSERTS throws out of the declaration book's range check in the middle of a dispatch, and the drop that follows frees a bad pointer in ~command_list, inside mi_free.
In a full run the corruption surfaces elsewhere: in check_copy_outside_rendering_targets, in ~dx12_command_list, once as a 180 s hang.
So every shaped-graphics-test run fails here, and dev.py ship refuses.
It passes on debug-clang, and on b5120daab; git bisect names 9f645e58c (the webgpu mesh polyfill), the first commit that grew command_list's members.
The book's apply looks exception-safe, so the unwinding path through the dx12 dispatch is the next place to look.
Found while landing feat-sg-gpu-written-clusters, whose own tests pass.
