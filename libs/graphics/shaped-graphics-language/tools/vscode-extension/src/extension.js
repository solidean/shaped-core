// The SGL extension's client half: it finds a built `sgl`, starts `sgl lsp` over stdio, and draws what the server reports.
// The readme's "Editor support (VS Code)" section is the user-facing description.

'use strict';

const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;
let output;
let extensionContext;

// Per watched binary path: the mtime it had when the server started, the last mtime a prompt offered, and its settle timer.
const watched = new Map();

// Every start, stop and restart runs on this one chain, so two of them never overlap and leak a second client.
let lifecycle = Promise.resolve();

// ---------------------------------------------------------------------------------------------------------------------
// Finding and copying the binary
// ---------------------------------------------------------------------------------------------------------------------

function log(message) {
    output.appendLine(`[client] ${message}`);
}

function statOrNull(file) {
    try {
        return fs.statSync(file);
    } catch {
        return null;
    }
}

// The build directory of the preset `uv run dev.py build` uses when given none, per platform.
const DEFAULT_BUILD_DIRS = {
    linux: 'x64-linux-clang-ninja-relwithdebinfo',
    win32: 'x64-windows-clang-ninja-relwithdebinfo',
    darwin: 'macos-arm-llvm-relwithdebinfo',
};

// Whether a build directory's binaries can run here, judged by the preset naming only.
// A name the presets do not use counts as native, since it may be a build a user configured by hand.
function isNativeBuildDir(name) {
    if (/^(wasm|android|ios)-/.test(name)) {
        return false;
    }
    if (/^macos-/.test(name)) {
        return process.platform === 'darwin';
    }
    const m = /^(x64|arm64)-(windows|linux)-/.exec(name);
    if (m) {
        const os = m[2] === 'windows' ? 'win32' : 'linux';
        return m[1] === process.arch && os === process.platform;
    }
    return true;
}

// The `sgl` a build directory's nexus manifest names, whether or not it exists yet; undefined without a manifest.
function manifestBinary(buildDir) {
    try {
        const binary = JSON.parse(fs.readFileSync(path.join(buildDir, 'nexus-binaries.json'), 'utf8'))?.paths?.sgl;
        // An emscripten build names a `.js` launcher, which no editor can run as a server.
        return typeof binary === 'string' && !/\.(js|html|wasm)$/i.test(binary) ? binary : undefined;
    } catch {
        return undefined;
    }
}

// The `sgl` binary to run, and every binary whose rebuild should offer a restart.
// The setting wins when set; else the default preset's build, else the newest native one any manifest names.
// Returns undefined, having said why, when there is none.
function resolveSourceBinary() {
    const configured = vscode.workspace.getConfiguration('sgl').get('server.path', '').trim();
    if (configured) {
        if (statOrNull(configured)?.isFile()) {
            return { binary: configured, reason: 'the sgl.server.path setting', watch: [configured] };
        }
        vscode.window.showErrorMessage(`SGL: sgl.server.path points at "${configured}", which is not a file.`);
        return undefined;
    }

    const defaults = [];
    const others = [];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        if (folder.uri.scheme !== 'file') {
            continue;
        }
        const buildRoot = path.join(folder.uri.fsPath, 'build');
        let dirs = [];
        try {
            dirs = fs.readdirSync(buildRoot, { withFileTypes: true }).filter((e) => e.isDirectory());
        } catch {
            continue;
        }
        for (const dir of dirs) {
            if (!isNativeBuildDir(dir.name)) {
                continue;
            }
            const binary = manifestBinary(path.join(buildRoot, dir.name));
            if (!binary) {
                continue;
            }
            const isDefault = dir.name === DEFAULT_BUILD_DIRS[process.platform];
            const stat = statOrNull(binary);
            // The default preset's binary is watched even before it exists, so building it later offers a restart.
            (isDefault ? defaults : others).push({ binary, mtimeMs: stat?.isFile() ? stat.mtimeMs : -1 });
        }
    }

    const newest = (list) => list.filter((c) => c.mtimeMs >= 0).sort((a, b) => b.mtimeMs - a.mtimeMs)[0];
    const watch = defaults.map((c) => c.binary);
    const preferred = newest(defaults);
    if (preferred) {
        return { binary: preferred.binary, reason: 'the default preset', watch };
    }
    const fallback = newest(others);
    if (fallback) {
        return {
            binary: fallback.binary,
            reason: 'the newest native build, since the default preset has none',
            watch: [fallback.binary, ...watch],
        };
    }

    vscode.window.showErrorMessage(
        'SGL: no built `sgl` found. Build it with `uv run dev.py build -t sgl` in the shaped-core workspace, ' +
            'or set `sgl.server.path` to an sgl binary.'
    );
    return undefined;
}

// Copies `source` (and a sibling .pdb) into a directory of the extension's storage keyed by the source's mtime.
// The server runs from the copy, so a rebuild can overwrite the original while the server holds its file open.
function copyBinary(source) {
    const mtimeMs = Math.floor(fs.statSync(source).mtimeMs);
    const storage = extensionContext.globalStorageUri.fsPath;
    const dirName = `sgl-${mtimeMs}`;
    const dir = path.join(storage, dirName);
    const target = path.join(dir, path.basename(source));

    // The pdb keeps its own name, since that is the name the debugger looks for next to the executable.
    const pdb = source.replace(/\.exe$/i, '.pdb');
    const hasPdb = process.platform === 'win32' && pdb !== source && statOrNull(pdb)?.isFile();

    if (!statOrNull(target)?.isFile()) {
        fs.mkdirSync(dir, { recursive: true });
        const partial = `${target}.partial`;
        fs.copyFileSync(source, partial);
        if (hasPdb) {
            fs.copyFileSync(pdb, path.join(dir, path.basename(pdb)));
        }
        fs.renameSync(partial, target);
        if (process.platform !== 'win32') {
            fs.chmodSync(target, 0o755);
        }
        log(`copied ${source} to ${target}`);
    }

    // Best effort: a copy still running under another window is busy, and the next start retries it.
    for (const entry of fs.readdirSync(storage, { withFileTypes: true })) {
        if (entry.isDirectory() && /^sgl-\d+$/.test(entry.name) && entry.name !== dirName) {
            try {
                fs.rmSync(path.join(storage, entry.name), { recursive: true, force: true });
            } catch {
                // EBUSY / EPERM: in use elsewhere.
            }
        }
    }

    return { binary: target, mtimeMs };
}

// ---------------------------------------------------------------------------------------------------------------------
// Watching for a newer build
// ---------------------------------------------------------------------------------------------------------------------

const POLL_MS = 2000;
// A link writes the file more than once, so a prompt waits until two polls in a row saw no change.
const SETTLE_MS = 5000;

function mtimeOf(file) {
    const stat = statOrNull(file);
    return stat?.isFile() ? Math.floor(stat.mtimeMs) : -1;
}

function unwatchAll() {
    for (const [file, w] of watched) {
        fs.unwatchFile(file, w.listener);
        clearTimeout(w.timer);
    }
    watched.clear();
}

function watchBinaries(files) {
    unwatchAll();
    for (const file of new Set(files)) {
        const w = { startMtimeMs: mtimeOf(file), notifiedMtimeMs: -1, timer: undefined };
        w.listener = () => {
            clearTimeout(w.timer);
            w.timer = setTimeout(() => onBinarySettled(file, w), SETTLE_MS);
        };
        watched.set(file, w);
        fs.watchFile(file, { interval: POLL_MS, persistent: false }, w.listener);
    }
}

async function onBinarySettled(file, w) {
    const mtimeMs = mtimeOf(file);
    if (mtimeMs < 0 || mtimeMs === w.startMtimeMs || mtimeMs === w.notifiedMtimeMs) {
        return;
    }
    w.notifiedMtimeMs = mtimeMs;
    const choice = await vscode.window.showInformationMessage('A newer sgl was built.', 'Restart');
    if (choice === 'Restart') {
        await restartServer();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Starting and stopping the client
// ---------------------------------------------------------------------------------------------------------------------

// Queues `step` behind every start, stop and restart already asked for; a failed step does not block the next.
function serialized(what, step) {
    lifecycle = lifecycle.then(step).catch((e) => log(`${what} failed: ${e?.message ?? e}`));
    return lifecycle;
}

async function doStart() {
    const resolved = resolveSourceBinary();
    if (!resolved) {
        return;
    }
    const source = resolved.binary;
    log(`using ${source} (${resolved.reason})`);

    let copy;
    try {
        copy = copyBinary(source);
    } catch (e) {
        vscode.window.showErrorMessage(`SGL: could not copy ${source}: ${e.message}`);
        return;
    }
    watchBinaries(resolved.watch);

    const logLevel = vscode.workspace.getConfiguration('sgl').get('server.logLevel', 'info');
    const serverOptions = {
        command: copy.binary,
        args: ['lsp'],
        transport: TransportKind.stdio,
    };
    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'sgl' }],
        initializationOptions: { logLevel },
        outputChannel: output,
    };

    client = new LanguageClient('sgl', 'SGL Language Server', serverOptions, clientOptions);
    client.onNotification('sgl/checkResults', onCheckResults);
    log(`starting ${copy.binary} lsp (built from ${source})`);
    try {
        await client.start();
    } catch (e) {
        vscode.window.showErrorMessage(`SGL: the language server failed to start: ${e.message}`);
    }
}

async function doStop() {
    const stopping = client;
    client = undefined;
    if (stopping) {
        try {
            await stopping.stop();
        } catch (e) {
            log(`stopping the server failed: ${e.message}`);
        }
    }
}

function startServer() {
    return serialized('starting the server', doStart);
}

function stopServer() {
    return serialized('stopping the server', doStop);
}

function restartServer() {
    return serialized('restarting the server', async () => {
        await doStop();
        await doStart();
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Check marks
// ---------------------------------------------------------------------------------------------------------------------

let decorations;

// Per document uri: the results version, its marks, whether an edit has outdated them, and the pending stale timer.
const checkMarks = new Map();

function createDecorations(context) {
    const make = (name) =>
        vscode.window.createTextEditorDecorationType({
            gutterIconPath: context.asAbsolutePath(path.join('icons', `${name}.svg`)),
            gutterIconSize: 'contain',
        });
    decorations = {
        pass: make('pass'),
        fail: make('fail'),
        mixed: make('mixed'),
        notrun: make('notrun'),
        stale: make('stale'),
    };
    for (const d of Object.values(decorations)) {
        context.subscriptions.push(d);
    }
}

function markKind(mark) {
    if (mark.passed === 0 && mark.failed === 0) {
        return 'notrun';
    }
    if (mark.failed === 0) {
        return 'pass';
    }
    return mark.passed === 0 ? 'fail' : 'mixed';
}

function markSummary(mark) {
    if (mark.passed === 0 && mark.failed === 0) {
        return 'never ran';
    }
    return `passed ${mark.passed}, failed ${mark.failed}`;
}

function applyMarks(editor) {
    const entry = checkMarks.get(editor.document.uri.toString());
    const byKind = { pass: [], fail: [], mixed: [], notrun: [], stale: [] };

    for (const mark of entry?.marks ?? []) {
        const line = mark.range.start.line;
        if (line >= editor.document.lineCount) {
            continue;
        }
        // One gutter icon per site, so the decoration covers only the site's first line.
        const end = mark.range.end.line === line ? mark.range.end.character : editor.document.lineAt(line).range.end.character;
        const range = new vscode.Range(line, mark.range.start.character, line, end);
        const summary = markSummary(mark);
        const kind = entry.stale ? 'stale' : markKind(mark);
        byKind[kind].push({
            range,
            hoverMessage: entry.stale ? `outdated by an edit — last run: ${summary}` : summary,
        });
    }

    for (const [kind, type] of Object.entries(decorations)) {
        editor.setDecorations(type, byKind[kind]);
    }
}

function applyMarksFor(uri) {
    for (const editor of vscode.window.visibleTextEditors) {
        if (editor.document.uri.toString() === uri) {
            applyMarks(editor);
        }
    }
}

// Per document uri: every edit since the results shown, as `{ version, changes }` with the version it produced.
// Results for an older version replay the edits after it, so their marks land where the edited text moved them.
const editLogs = new Map();
const EDIT_LOG_LIMIT = 2000;

function isBefore(a, b) {
    return a.line < b.line || (a.line === b.line && a.character < b.character);
}

// Where `pos` ends up once `change` replaced its range; a position inside the replaced text collapses to its start.
function shiftPosition(pos, change) {
    if (isBefore(pos, change.start)) {
        return pos;
    }
    if (isBefore(pos, change.end)) {
        return change.start;
    }
    const newlines = change.text.split('\n').length - 1;
    const line = pos.line + newlines - (change.end.line - change.start.line);
    if (pos.line !== change.end.line) {
        return { line, character: pos.character };
    }
    const tail =
        newlines === 0 ? change.start.character + change.text.length : change.text.length - change.text.lastIndexOf('\n') - 1;
    return { line, character: tail + pos.character - change.end.character };
}

// `marks` moved through `changes` in order, the way VS Code moves a decoration through the same edit.
function shiftMarks(marks, changes) {
    for (const change of changes) {
        marks = marks.map((mark) => ({
            ...mark,
            range: { start: shiftPosition(mark.range.start, change), end: shiftPosition(mark.range.end, change) },
        }));
    }
    return marks;
}

function plainChanges(contentChanges) {
    return contentChanges.map((c) => ({
        start: { line: c.range.start.line, character: c.range.start.character },
        end: { line: c.range.end.line, character: c.range.end.character },
        text: c.text,
    }));
}

function onCheckResults(params) {
    const uri = params.uri;
    const document = vscode.workspace.textDocuments.find((d) => d.uri.toString() === uri);
    if (!document) {
        // Nothing draws marks for a closed document, and nothing would delete them.
        return;
    }
    const previous = checkMarks.get(uri);
    if (previous && params.version < previous.version) {
        return;
    }
    clearTimeout(previous?.timer);

    const later = (editLogs.get(uri) ?? []).filter((e) => e.version > params.version);
    editLogs.set(uri, later);
    const marks = shiftMarks(params.marks ?? [], later.flatMap((e) => e.changes));

    const entry = { version: params.version, marks, stale: false };
    // Results for a version the document has already left behind start stale, unless an edit's own debounce is running.
    if (document.version > params.version) {
        if (previous?.timer) {
            entry.timer = setTimeout(() => markStale(uri), 400);
        } else {
            entry.stale = true;
        }
    }
    checkMarks.set(uri, entry);
    applyMarksFor(uri);
}

function markStale(uri) {
    const entry = checkMarks.get(uri);
    if (!entry) {
        return;
    }
    entry.timer = undefined;
    if (!entry.stale) {
        entry.stale = true;
        applyMarksFor(uri);
    }
}

function onDocumentChanged(event) {
    if (event.contentChanges.length === 0 || event.document.languageId !== 'sgl') {
        return;
    }
    const uri = event.document.uri.toString();
    const changes = plainChanges(event.contentChanges);

    const edits = editLogs.get(uri) ?? [];
    edits.push({ version: event.document.version, changes });
    if (edits.length > EDIT_LOG_LIMIT) {
        edits.shift();
    }
    editLogs.set(uri, edits);

    const entry = checkMarks.get(uri);
    if (!entry) {
        return;
    }
    // VS Code has moved the drawn decorations already; the stored ranges follow, so a redraw keeps them where they are.
    entry.marks = shiftMarks(entry.marks, changes);
    clearTimeout(entry.timer);
    entry.timer = setTimeout(() => markStale(uri), 400);
}

function onDocumentClosed(document) {
    const uri = document.uri.toString();
    clearTimeout(checkMarks.get(uri)?.timer);
    checkMarks.delete(uri);
    editLogs.delete(uri);
}

// ---------------------------------------------------------------------------------------------------------------------
// Prelude documents
// ---------------------------------------------------------------------------------------------------------------------

const preludeProvider = {
    async provideTextDocumentContent(uri) {
        if (!client) {
            return '// The SGL language server is not running.';
        }
        const result = await client.sendRequest('sgl/preludeText', { uri: uri.toString() });
        return result?.text ?? '';
    },
};

function onDocumentOpened(document) {
    if (document.uri.scheme === 'sgl-prelude' && document.languageId !== 'sgl') {
        vscode.languages.setTextDocumentLanguage(document, 'sgl');
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------------------------------------------------

async function activate(context) {
    extensionContext = context;
    output = vscode.window.createOutputChannel('SGL Language Server');
    context.subscriptions.push(output);
    createDecorations(context);

    context.subscriptions.push(
        vscode.commands.registerCommand('sgl.restartServer', restartServer),
        vscode.workspace.registerTextDocumentContentProvider('sgl-prelude', preludeProvider),
        vscode.workspace.onDidOpenTextDocument(onDocumentOpened),
        vscode.workspace.onDidChangeTextDocument(onDocumentChanged),
        vscode.workspace.onDidCloseTextDocument(onDocumentClosed),
        vscode.window.onDidChangeVisibleTextEditors((editors) => editors.forEach(applyMarks)),
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration('sgl.server')) {
                restartServer();
            }
        }),
        { dispose: unwatchAll }
    );

    await startServer();
}

async function deactivate() {
    unwatchAll();
    await stopServer();
}

module.exports = { activate, deactivate };
