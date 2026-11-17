# GPU capture

One epoch of a program, captured in RenderDoc or PIX by setting an environment variable and running it as usual.
Nothing in the program changes, so a test, an example or a viewer is capturable headless and under `dev.py`.

```bash
SC_SG_CAPTURE=renderdoc SC_SG_CAPTURE_EPOCH=3 SC_SG_CAPTURE_FILE=<dir>/cube uv run dev.py example sgl-cube --capture
```

## The variables

| Variable | Value | Default |
|---|---|---|
| `SC_SG_CAPTURE` | `renderdoc` or `pix` | nothing is captured |
| `SC_SG_CAPTURE_EPOCH` | how many `advance_epoch` calls come before the captured epoch | `1` |
| `SC_SG_CAPTURE_FILE` | where the capture goes | the tool's own choice for RenderDoc, `sg-capture.wpix` for PIX |

**The captured epoch is the one the n-th advance opens**, `epoch::first + n`, and the capture ends at the next advance or when the context shuts down.
The epoch a context opens at creation cannot be captured, since the tool begins once a device exists; `0` is refused with a warning.
An example's first frames often build pipelines, so a frame a few epochs in is the one that draws.

**The first context to open that epoch owns the capture, and it is the only one per process.**
A program that creates several contexts captures one of them.

**For RenderDoc, the file is a path template**: RenderDoc appends its own suffix and `.rdc`.
The log names the file it wrote, at `info` under the `sg` domain.
For PIX it is the `.wpix` file itself.

## How it works

The tool's library must be in the process before the first device exists, or it hooks nothing.
Every backend creates and destroys its devices under `sg::impl::device_lifecycle_hold`, so the hold's first construction loads the tool, once per process.
No backend knows about capturing.

- **RenderDoc** is loaded from the process if something injected it already, then from the `PATH`, then from `%ProgramFiles%\RenderDoc\`; on Linux it is `librenderdoc.so`.
  The capture brackets the epoch with the in-application API's `StartFrameCapture` and `EndFrameCapture`, with no device named, which RenderDoc takes as the one device there is.
  It captures dx12 and vulkan alike.
- **PIX** is `WinPixGpuCapturer.dll` from the newest `%ProgramFiles%\Microsoft PIX\<version>\`, as pix3_win.h's loader picks it.
  The capture brackets the epoch with the two exports PIX's programmatic capture calls, so WinPixEventRuntime is not needed.
  PIX captures dx12 alone; on another backend it warns and captures nothing.

A tool that is asked for and cannot be loaded is a warning, and the program runs uncaptured.

## Reading a capture from a script

RenderDoc's replay API is reachable from the Python its UI embeds: `qrenderdoc --python <script.py>` runs the script before the UI opens, and `os._exit` at its end closes it again.
That is how a shader debug session is scripted: `DebugPixel` or `DebugThread`, then `ContinueDebug` until it returns nothing, reading each step's source line and variables.
[tools/gpu-capture/debug-pixel.py](../../tools/gpu-capture/debug-pixel.py) does that for one pixel and prints JSON.
It names each stage's debug files, the source variables, and every step's file, line, call stack and variables.

```bash
uv run libs/graphics/shaped-graphics/tools/gpu-capture/debug-pixel.py <capture.rdc> <x> <y> [--draw N] [--out result.json]
```

A trace's `instInfo` holds only the instructions with debug info, each naming its own instruction, so a step is looked up by that number rather than by position.
PIX's API debugs shaders only from a postmortem dump, so a PIX capture is for looking at by hand.
