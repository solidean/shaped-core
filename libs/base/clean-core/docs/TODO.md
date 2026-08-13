# clean-core TODO

Running list of known follow-ups.
Add entries as we discover them, and remove them as they land.

## common

- **Enum value names, through `cc::custom::enum_traits`.**
  A first version prints `enum_name(integer_value)`, and `cc::flags::to_string` then prints a comma-separated list of them.
  That needs a variadic enumerator list in the macro plus a name table on the traits, next to `is_flag_enum` and `flag_storage_type` — which is the shape the traits already have.
  The catch is that only ONE specialization of `cc::custom::enum_traits<E>` may exist, so a names-only macro cannot sit alongside a `CC_FLAG_ENUM_*` one for the same enum.
  The two have to converge into one macro that declares everything, with `is_flag_enum` telling a nameable enum from a flag enum.
- **Migrate the remaining hand-rolled flag enums to `cc::flags`.**
  `sg::buffer_usage`, `sg::texture_usage`, `sg::access_flags`, `sg::pipeline_stage_flags`, `sg::color_write_mask`, `sg::accel_build_flags` and `sr::key_modifiers` still carry their own operators.
  Retiring `sg::has_flag` along with them touches ~50 call sites, most of them inside `CC_ASSERT`.

## container

- **The stubs.**
  `ringbuffer`, `bitset`, `fixed_bitset` and `disjoint_set` are declared but not implemented.
- **Grow `tuple` and `variant`.**
  The first version deliberately left out converting construction from another `tuple<Us...>`, `tuple_cat`, `variant`'s `operator<=>` and multi-variant visitation.
  A `to_string` hidden friend for `variant` is missing too — it would drag `to_debug_string.hh` into a container header, so a `variant` currently debug-prints as a raw byte dump.
