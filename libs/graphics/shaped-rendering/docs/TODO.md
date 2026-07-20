# shaped-rendering TODO

Running list of known follow-ups. Bigger design intent lives in [structure.md](structure.md).

- First routines on the framework: mipmap generation, texture compression, tonemapping.
- Concrete routines currently need dx12 + DXC (Windows) for a real shader; the framework itself is
  cross-platform. Broaden once a non-Windows shader path exists.
- The routine library watches `std::shared_ptr<slib::shader_library>`; `add_shader_library` accepts any
  number, though slib currently allows one alive at a time.
- Settle the module layout once the first routines land, and grow the
  [cheat-sheet](../cheat-sheet.md) + [structure](structure.md) accordingly.
