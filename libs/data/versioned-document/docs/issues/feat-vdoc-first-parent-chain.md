---
created: 2026-10-17
refs:
  - file: examples/vdoc/cube-editor/cube_document.hh
  - symbol: vdoc::op_graph
---

# `op_graph::first_parent_chain`

`cc::vector<op_id> first_parent_chain(op_id) const` on `op_graph` would be about 20 lines.
It would serve every undo and history UI, instead of each one rebuilding the walk, as the cube-editor example does today.

A single-user file is a single-parent chain, so first-parent ancestry is its history.
The op DAG itself has no linear order: `collect_reachable` sorts by id bytes and `children` is arrival order.
