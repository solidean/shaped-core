# SGL TODO

Work that is decided and not built, as opposed to [spec/incubator/](spec/_index.md), which holds ideas nobody has decided yet.
`DEBUG_` in a name marks a stand-in for something listed here.

* **Values as type arguments.** `image2d[.rgba8_unorm]` takes an enum case, and the checker reads exactly that argument today, as a special case of image types.
  The general feature is a type parameterized on an integer or an enum value, which math templated on a dimension wants as well, and it lets code branch on the value.
* **Texture methods.** The `DEBUG_sample`, `DEBUG_sample_level`, `DEBUG_load`, `DEBUG_store` and `DEBUG_size` builtins stand in for them.
  The intended shape is methods through UFCS, with default and named arguments ([texture-methods.md](spec/incubator/texture-methods.md)).
* **Feature opt-in.** Every form [bindings.md](spec/bindings.md#features) refuses by feature is waiting for it.
  A function declares the features it needs ([feature-levels.md](spec/incubator/feature-levels.md)).
