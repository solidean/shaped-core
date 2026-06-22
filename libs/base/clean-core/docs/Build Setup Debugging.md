# Build Setup Debugging

TODO: this is currently just a braindump, format me properly.


clangd issues:
add "--log=verbose" to settings.json
go to vscode -> output panel -> top right choose "clangd"
(basically trigger your issue, then go to the panel and copy everything out. finds a lot of things)

there seems to be a clang-cl bug that eats c++latest
workaround:
vscode > clangd: Open user configuration file
add:
```yaml
CompileFlags:
  Add: [/std:c++latest]
```
