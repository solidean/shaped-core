---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/common/enum_traits.hh
  - file: libs/base/clean-core/src/clean-core/common/flags.hh
  - symbol: cc::custom::enum_traits
  - symbol: cc::flags
---

# Enum value names, through `cc::custom::enum_traits`

A first version prints `enum_name(integer_value)`, and `cc::flags::to_string` then prints a comma-separated list of them.
That needs a variadic enumerator list in the macro plus a name table on the traits, next to `is_flag_enum` and `flag_storage_type` — which is the shape the traits already have.

The catch is that only ONE specialization of `cc::custom::enum_traits<E>` may exist, so a names-only macro cannot sit alongside a `CC_FLAG_ENUM_*` one for the same enum.
The two have to converge into one macro that declares everything, with `is_flag_enum` telling a nameable enum from a flag enum.
