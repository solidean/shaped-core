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

// The binary the running server was copied from, and the mtime of the build it was copied at.
let sourceBinary;
let sourceMtimeMs = 0;
let notifiedMtimeMs = 0;
let watchTimer;

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

// The `sgl` binary to run: the setting when it is set, else the newest one any workspace build's nexus manifest names.
// Returns undefined, having said why, when there is none.
function resolveSourceBinary() {
    const configured = vscode.workspace.getConfiguration('sgl').get('server.path', '').trim();
    if (configured) {
        if (statOrNull(configured)?.isFile()) {
            return configured;
        }
        vscode.window.showErrorMessage(`SGL: sgl.server.path points at "${configured}", which is not a file.`);
        return undefined;
    }

    const candidates = [];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        if (folder.uri.scheme !== 'file') {
            continue;
        }
        const buildRoot = path.join(folder.uri.fsPath, 'build');
        let presets = [];
        try {
            presets = fs.readdirSync(buildRoot, { withFileTypes: true }).filter((e) => e.isDirectory());
        } catch {
            continue;
        }
        for (const preset of presets) {
            const manifest = path.join(buildRoot, preset.name, 'nexus-binaries.json');
            let binary;
            try {
                binary = JSON.parse(fs.readFileSync(manifest, 'utf8'))?.paths?.sgl;
            } catch {
                continue;
            }
            const stat = typeof binary === 'string' ? statOrNull(binary) : null;
            if (stat?.isFile()) {
                candidates.push({ binary, mtimeMs: stat.mtimeMs });
            }
        }
    }

    if (candidates.length === 0) {
        vscode.window.showErrorMessage(
            'SGL: no built `sgl` found. Build it with `uv run dev.py build -t sgl` in the shaped-core workspace, ' +
                'or set `sgl.server.path` to an sgl binary.'
        );
        return undefined;
    }
    candidates.sort((a, b) => b.mtimeMs - a.mtimeMs);
    return candidates[0].binary;
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

function unwatchSource() {
    if (sourceBinary) {
        fs.unwatchFile(sourceBinary, onSourceChanged);
    }
    clearTimeout(watchTimer);
}

function watchSource(binary) {
    unwatchSource();
    sourceBinary = binary;
    fs.watchFile(binary, { interval: 2000, persistent: false }, onSourceChanged);
}

function onSourceChanged() {
    // A link writes the file more than once, so the prompt waits for the mtime to settle.
    clearTimeout(watchTimer);
    watchTimer = setTimeout(async () => {
        const stat = statOrNull(sourceBinary);
        if (!stat) {
            return;
        }
        const mtimeMs = Math.floor(stat.mtimeMs);
        if (mtimeMs === sourceMtimeMs || mtimeMs === notifiedMtimeMs) {
            return;
        }
        notifiedMtimeMs = mtimeMs;
        const choice = await vscode.window.showInformationMessage('A newer sgl was built.', 'Restart');
        if (choice === 'Restart') {
            await restartServer();
        }
    }, 1500);
}

// ---------------------------------------------------------------------------------------------------------------------
// Starting and stopping the client
// ---------------------------------------------------------------------------------------------------------------------

async function startServer() {
    const source = resolveSourceBinary();
    if (!source) {
        return;
    }

    let copy;
    try {
        copy = copyBinary(source);
    } catch (e) {
        vscode.window.showErrorMessage(`SGL: could not copy ${source}: ${e.message}`);
        return;
    }
    sourceMtimeMs = copy.mtimeMs;
    watchSource(source);

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

async function stopServer() {
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

async function restartServer() {
    await stopServer();
    await startServer();
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

function onCheckResults(params) {
    const uri = params.uri;
    const previous = checkMarks.get(uri);
    if (previous && params.version < previous.version) {
        return;
    }
    clearTimeout(previous?.timer);

    const document = vscode.workspace.textDocuments.find((d) => d.uri.toString() === uri);
    const entry = { version: params.version, marks: params.marks ?? [], stale: false };
    // Results for a version the document has already left behind start stale, unless an edit's own debounce is running.
    if (document && document.version > params.version) {
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
    const uri = event.document.uri.toString();
    const entry = checkMarks.get(uri);
    if (!entry || event.contentChanges.length === 0) {
        return;
    }
    clearTimeout(entry.timer);
    entry.timer = setTimeout(() => markStale(uri), 400);
}

function onDocumentClosed(document) {
    const uri = document.uri.toString();
    clearTimeout(checkMarks.get(uri)?.timer);
    checkMarks.delete(uri);
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
        { dispose: unwatchSource }
    );

    await startServer();
}

async function deactivate() {
    unwatchSource();
    await stopServer();
}

module.exports = { activate, deactivate };
