# __project__

Built on [shaped-core](extern/shaped-core/README.md), a git submodule at `extern/shaped-core`.

## Quick start

Prerequisites are shaped-core's: a C++23 toolchain, CMake >= 3.28, and [`uv`](https://docs.astral.sh/uv/).

```bash
git submodule update --init --recursive
uv run dev.py doctor   # sanity-check the toolchain
uv run dev.py test     # configure, build and run the tests
```

`uv run dev.py submodules sync` after every pull keeps `extern/shaped-core` at the commit this repository records.

## Layout

```
libs/<category>/<lib>   # the libraries: src/<lib>/, tests/, optional docs/
apps/<app>              # executables, thin over the libraries
docs/                   # repo-wide docs, start at docs/_index.md; docs/issues/ is the issue tracker
extern/shaped-core      # shaped-core, a submodule; its dev.py driver is reused, not copied
dev.py                  # build & test driver: this project's policy over shaped-core's driver
```
