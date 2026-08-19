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
  `sr::key_modifiers` is the last one still carrying its own operators plus a free `has_all`.
  All of sg's have migrated — `buffer_usages`, `texture_usages`, `access_flags`, `pipeline_stage_flags`, `color_write_mask`, `accel_build_flags`.

- **Revisit the regular-void stance across the vocabulary types.**
  Today `void` is rejected and you spell `cc::unit` by hand: `result<void, E>` is a static_assert pointing at it, `optional<void>` has the same shape, and `async<void>` was never wired up at all.
  Generic code that forms `xyz<T>` where `T` can reasonably be `void` hits this everywhere, so a per-type alias fixes one corner and leaves the rest.
  The direction is to support `xyz<void>` as an affordance meaning `xyz<unit>` — same type, no conversions — decided once for `result`, `optional` and `async` together rather than type by type.
  Two consequences the async side already ran into, and that the decision has to answer.
  A `shared_async<void>` dependency should contribute an ordering edge and NO argument to the `make_async_*` sugar.
  And it becomes indistinguishable from one deliberately carrying a `unit`.

## container

- **`bitset` printing and allocation interop.**
  Neither bit set has a `to_string`: it would drag `cc::string` into a container header, the way `variant`'s missing one does.
  The bit order is already decided — **index 0 leftmost**, since a bit set is an indexed bit array rather than a number.
  `cc::bitset` also cannot yet adopt or extract its `cc::allocation<u64>`.
- **`ringbuffer` adoption and extraction.**
  It holds its `cc::allocation<T>` as a raw byte handle, so neither `create_from_allocation` nor `extract_allocation` exists yet.
  Adoption can take the largest power of two at or below the incoming capacity; extraction needs the content linearized first, or a check that it already is.
- **Grow `tuple` and `variant`.**
  The first version deliberately left out converting construction from another `tuple<Us...>`, `tuple_cat`, `variant`'s `operator<=>` and multi-variant visitation.
  A `to_string` hidden friend for `variant` is missing too — it would drag `to_debug_string.hh` into a container header, so a `variant` currently debug-prints as a raw byte dump.
