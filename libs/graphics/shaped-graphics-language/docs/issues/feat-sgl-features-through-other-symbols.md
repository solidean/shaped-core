---
created: 2026-10-17
parent: epic-sgl-feature-grants
refs:
  - symbol: checker::compile
---

# Features used through another symbol

A binding's `required` counts only the uses resolved while its members compile, and `checker::compile` clears the grant around any symbol they demand.
No such symbol can hold a resource yet.
Once a type alias or a struct field can, its use has to reach every binding that names it.
