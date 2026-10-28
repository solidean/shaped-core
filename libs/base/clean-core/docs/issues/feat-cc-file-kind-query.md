---
created: 2026-10-28
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/uri_resolver.cc
  - symbol: sv::impl::names_windows_device
---

# A file-kind query, and a Windows device-name rule for paths

clean-core can open a file but cannot say what a path names before it does.
A reader that must refuse anything but a regular file has to infer it from the stream.
shaped-viewer's `sv::impl::resolve_uri_from_filesystem` does that today: seekable, and reading yields exactly its stated size.

That heuristic cannot see a named pipe.
`native_file::open` passes `O_RDONLY` without `O_NONBLOCK`, so on Linux and macOS opening a FIFO for reading blocks until a writer appears.
A texture beside a document that is a FIFO (a tar archive can carry one) hangs the import worker forever, and a dropped `sv::asset` waits for it.

Wanted:

- **A file-kind query** answering regular / directory / device / pipe / other for a path, before anything is opened.
  It must not block on a FIFO: `stat` before open, or open with `O_NONBLOCK`.
- **`names_windows_device` as a path helper**: whether Windows would open a path as a device (`\\.\`, `CON`, `NUL`, `COM1`, `CONIN$`, … before the first dot or colon).
  `NUL` opens, states a size of 0 and reads as empty, so only its name gives it away.
  It is judged the same on every platform, since a name authored anywhere may be opened on Windows.

Once both exist, shaped-viewer's two local copies go, and its filesystem resolver asks the query instead of inferring.
