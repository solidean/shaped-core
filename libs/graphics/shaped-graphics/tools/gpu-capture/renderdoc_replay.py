# Runs inside RenderDoc's UI process, `qrenderdoc --python`, whose embedded Python has the `renderdoc` module.
# debug-pixel.py starts it; the request and the result travel through two files named by environment variables.
#
# The request is JSON: the capture, a pixel, and optionally which draw (by default the last).
# The result is JSON: each stage's debug info, the source variables, and every step's line and call stack.

import json
import os
import traceback

import renderdoc as rd


def draws_of(controller):
    found = []

    def walk(actions):
        for a in actions:
            if a.flags & rd.ActionFlags.Drawcall:
                found.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    return found


def stage_info(state, stage):
    refl = state.GetShaderReflection(stage)
    if refl is None:
        return None
    dbg = refl.debugInfo
    return {
        "entry": refl.entryPoint,
        "encoding": str(refl.encoding),
        "debuggable": bool(dbg.debuggable),
        "debug_status": dbg.debugStatus,
        "files": [{"name": f.filename, "size": len(f.contents)} for f in dbg.files],
    }


def variable_names(variables):
    return [{"name": v.name, "type": str(v.type), "members": variable_names(v.members)} for v in variables]


def debug_pixel(controller, request, result):
    draws = draws_of(controller)
    result["draws"] = len(draws)
    if not draws:
        result["error"] = "the capture holds no draw"
        return
    draw = draws[request.get("draw", -1)]
    result["draw"] = {"event": draw.eventId, "name": draw.GetName(controller.GetStructuredFile())}
    controller.SetFrameEvent(draw.eventId, True)

    state = controller.GetPipelineState()
    result["stages"] = {}
    for name, stage in (("vertex", rd.ShaderStage.Vertex), ("pixel", rd.ShaderStage.Pixel)):
        info = stage_info(state, stage)
        if info is not None:
            result["stages"][name] = info

    trace = controller.DebugPixel(request["x"], request["y"], rd.DebugPixelInputs())
    if trace.debugger is None:
        result["error"] = "no fragment covers the pixel"
        return
    result["source_vars"] = [
        {"name": v.name, "type": str(v.type), "rows": v.rows, "columns": v.columns} for v in trace.sourceVars
    ]
    files = result["stages"].get("pixel", {}).get("files", [])

    # instInfo holds an entry per instruction that has debug info, each naming its own instruction: no index into it
    info_of = {info.instruction: info for info in trace.instInfo}
    steps = []
    while True:
        states = controller.ContinueDebug(trace.debugger)
        if not states:
            break
        for s in states:
            step = {"instruction": s.nextInstruction, "callstack": list(s.callstack)}
            inst = info_of.get(s.nextInstruction)
            if inst is not None:
                line = inst.lineInfo
                step["file"] = files[line.fileIndex]["name"] if 0 <= line.fileIndex < len(files) else None
                step["line"] = line.lineStart
                step["vars"] = [
                    {"name": m.name, "type": str(m.type), "from": [v.name for v in m.variables]}
                    for m in inst.sourceVars
                ]
            step["changes"] = [c.after.name for c in s.changes if c.after.name]
            steps.append(step)
    result["steps"] = steps
    controller.FreeTrace(trace)


def main():
    with open(os.environ["SG_RENDERDOC_REQUEST"], encoding="utf-8") as f:
        request = json.load(f)
    result = {}
    try:
        cap = rd.OpenCaptureFile()
        if cap.OpenFile(request["capture"], "", None) != rd.ResultCode.Succeeded:
            result["error"] = "RenderDoc could not open the capture"
        else:
            status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
            if status != rd.ResultCode.Succeeded:
                result["error"] = "RenderDoc could not replay the capture: " + str(status)
            else:
                debug_pixel(controller, request, result)
                controller.Shutdown()
            cap.Shutdown()
    except Exception:
        result["error"] = traceback.format_exc()
    with open(os.environ["SG_RENDERDOC_RESULT"], "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)


main()
# the UI would open next; the script is all that was asked for
os._exit(0)
