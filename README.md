# shaped-core

Shaped Core is a collection of foundational C++ libraries developed by Shaped Code. They power
Solidean, internal tools, customer projects, and experimental research efforts.

The libraries are C++23, built with CMake, and share a single build & test driver
([dev.py](dev.py)). The set is **growing** — the table below is current, not exhaustive.

## What's inside

Libraries live under `libs/<category>/<lib>`:

| Library                       | What it is                                                                                  |
|-------------------------------|---------------------------------------------------------------------------------------------|
| [clean-core](libs/base/clean-core/) | Foundational C++ data structures, memory utilities, assertions, and low-level primitives (`span`, `vector`, `string`, `optional`, `result`, …). Namespace `cc`. |
| [nexus](libs/base/nexus/)      | Lightweight C++23 test framework, Catch2 v3 CLI–compatible (discovery, filtering, sections, JUnit XML). Namespace `nx`. |

See [docs/libraries.md](docs/libraries.md) for the full catalog.

## Quick start

Prerequisites: a C++23 toolchain (Clang or MSVC; GCC second-class), CMake >= 3.28, and
[`uv`](https://docs.astral.sh/uv/) (it runs the Python driver — no venv setup needed).

```bash
uv run dev.py doctor   # sanity-check the toolchain
uv run dev.py build    # configure + build (default preset for your platform)
uv run dev.py test     # build + run the full test suite
```

Run a subset while iterating:

```bash
uv run dev.py test "<pattern>"   # auto-build + run just the matching test(s)
```

`dev.py` auto-configures and auto-builds as needed, and is quiet by default (it captures logs
under `build/<preset>/` and prints a terse summary). See
[docs/guides/building-and-testing.md](docs/guides/building-and-testing.md) for the full reference.

## Build presets

Presets exist per platform and compiler (MSVC / Clang / GCC across Windows / Linux / macOS /
Android). A sensible default is chosen for your platform; override with `--preset` (after the
subcommand). List them with:

```bash
uv run dev.py list-presets
```

## Layout

```
libs/<category>/<lib>   # the libraries (src/<lib>/, tests/, optional docs/)
tools/                  # dev/ (build & test modules) and bin/ (checked-in binaries)
docs/                   # repo-wide docs — start at docs/_index.md
dev.py                  # build & test driver
CMakeLists.txt          # top-level build
CMakePresets.json       # platform/compiler presets
```

See [docs/_index.md](docs/_index.md) for documentation and [CLAUDE.md](CLAUDE.md) for the
working conventions of this repo.

## Contributing

* `main` is the integration branch. Feature branches are **mandatory** and namespaced per
  contributor by initials: `u/<your-initials>/<feature>`.
* Code style is enforced by [.clang-format](.clang-format); design conventions live in
  [docs/coding-guidelines.md](docs/coding-guidelines.md).

## License

MIT — see [LICENSE](LICENSE).
