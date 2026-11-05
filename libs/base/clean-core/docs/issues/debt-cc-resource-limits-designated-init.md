---
created: 2026-11-05
refs:
  - file: libs/base/clean-core/src/clean-core/platform/resource_limits.cc
---

# join_capped_job fills its job limits field by field

What is wrong or wanted, and what is already known.

`join_capped_job` in `platform/resource_limits.cc` declares its `JOBOBJECT_EXTENDED_LIMIT_INFORMATION` and fills it field by field.
That is against the designated-initializers-first rule of the coding guidelines.
New code, so it is migrated rather than left as history; found by a code review on 2026-11-05.
