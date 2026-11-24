import * as cp from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
} from 'vscode-languageclient/node';

let lspClient: LanguageClient | undefined;
let playerPanel: vscode.WebviewPanel | undefined;
let playerReady = false;
let pendingLoad: { name: string; data: number[] } | undefined;
let fastForwardActive = false;
let statusBarItem: vscode.StatusBarItem | undefined;

function isExecutableFile(p: string): boolean {
  try {
    return fs.statSync(p).isFile();
  } catch {
    return false;
  }
}

function findBinary(configKey: string, binName: string, context: vscode.ExtensionContext): string | undefined {
  const cfg = vscode.workspace.getConfiguration('mdxnt');
  const configured = cfg.get<string>(configKey)?.trim();
  const exe = process.platform === 'win32' ? `${binName}.exe` : binName;

  if (configured) {
    if (isExecutableFile(configured)) {
      return configured;
    }
    // Also allow bare binary names on PATH
    if (!configured.includes(path.sep) && !configured.includes('/')) {
      for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
        const candidate = path.join(dir, configured);
        if (isExecutableFile(candidate)) return candidate;
      }
    }
    vscode.window.showWarningMessage(`mdxnt.${configKey} is not a valid file: ${configured}`);
  }

  const candidates: string[] = [];

  // Relative to extension directory (tools/code -> tools/mdxnt-rs/target/...)
  candidates.push(path.resolve(context.extensionPath, '..', 'mdxnt-rs', 'target', 'release', exe));
  candidates.push(path.resolve(context.extensionPath, '..', 'mdxnt-rs', 'target', 'debug', exe));

  // Relative to open workspace folders
  for (const folder of vscode.workspace.workspaceFolders ?? []) {
    candidates.push(path.join(folder.uri.fsPath, 'tools', 'mdxnt-rs', 'target', 'release', exe));
    candidates.push(path.join(folder.uri.fsPath, 'tools', 'mdxnt-rs', 'target', 'debug', exe));
  }

  // Fallback to PATH
  for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
    if (dir) candidates.push(path.join(dir, exe));
  }

  for (const p of candidates) {
    if (isExecutableFile(p)) {
      return p;
    }
  }
  return undefined;
}

function compileMmlText(
  mmlText: string,
  context: vscode.ExtensionContext
): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const cliBin = findBinary('compilerPath', 'mdxnt', context);
    if (!cliBin) {
      reject(
        new Error(
          'mdxnt compiler binary not found. Build it with `cargo build --release` in tools/mdxnt-rs or set `mdxnt.compilerPath`.'
        )
      );
      return;
    }

    const proc = cp.spawn(cliBin, ['--stdout', '-'], {
      stdio: ['pipe', 'pipe', 'pipe'],
    });

    const stdoutChunks: Buffer[] = [];
    const stderrChunks: Buffer[] = [];
    let settled = false;

    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        proc.kill();
        reject(new Error('mdxnt compiler timed out after 15s'));
      }
    }, 15000);

    proc.stdin.on('error', () => {
      // Ignore EPIPE if child exits before consuming stdin
    });
    proc.stdout.on('data', (chunk: Buffer) => stdoutChunks.push(chunk));
    proc.stderr.on('data', (chunk: Buffer) => stderrChunks.push(chunk));
    proc.on('error', (err) => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        reject(err);
      }
    });
    proc.on('close', (code, signal) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (code === 0) {
        resolve(Buffer.concat(stdoutChunks));
      } else {
        const msg = Buffer.concat(stderrChunks).toString('utf8').trim();
        const reason = signal ? `killed by signal ${signal}` : `exited with code ${code}`;
        reject(new Error(msg || `mdxnt ${reason}`));
      }
    });

    proc.stdin.write(mmlText, 'utf8');
    proc.stdin.end();
  });
}

function getWebviewHtml(
  webview: vscode.Webview,
  context: vscode.ExtensionContext
): string {
  const mediaDir = vscode.Uri.joinPath(context.extensionUri, 'media');
  const playerJsUri = webview.asWebviewUri(vscode.Uri.joinPath(mediaDir, 'player.js'));
  const workletJsUri = webview.asWebviewUri(vscode.Uri.joinPath(mediaDir, 'worklet.js'));
  const wasmUri = webview.asWebviewUri(vscode.Uri.joinPath(mediaDir, 'mdxnt_player.wasm'));
  const nonce = String(Date.now()) + Math.random().toString(36).slice(2);

  return /* html */ `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <meta http-equiv="Content-Security-Policy"
        content="default-src 'none';
                 style-src ${webview.cspSource} 'unsafe-inline';
                 script-src 'nonce-${nonce}' ${webview.cspSource} 'wasm-unsafe-eval';
                 connect-src ${webview.cspSource} blob:;
                 worker-src ${webview.cspSource} blob:;" />
  <title>mdxnt YM2203 OPN Player</title>
  <style>
    :root {
      --bg: var(--vscode-editor-background);
      --fg: var(--vscode-editor-foreground);
      --muted: var(--vscode-descriptionForeground);
      --border: var(--vscode-panel-border, rgba(128,128,128,0.25));
      --btn-bg: var(--vscode-button-secondaryBackground, #3a3d41);
      --btn-fg: var(--vscode-button-secondaryForeground, #fff);
      --btn-hover: var(--vscode-button-secondaryHoverBackground, #45494e);
      --accent: var(--vscode-button-background, #0e639c);
      --accent-fg: var(--vscode-button-foreground, #fff);
      --card: var(--vscode-sideBar-background, rgba(128,128,128,0.08));
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      padding: 16px;
      background: var(--bg);
      color: var(--fg);
      font-family: var(--vscode-font-family, system-ui, sans-serif);
      font-size: 13px;
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      margin-bottom: 12px;
      border-bottom: 1px solid var(--border);
      padding-bottom: 8px;
    }
    .title { font-size: 15px; font-weight: 600; }
    .subtitle { color: var(--muted); font-size: 11px; }
    .card {
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 12px;
      margin-bottom: 12px;
    }
    .row {
      display: flex;
      gap: 8px;
      align-items: center;
      flex-wrap: wrap;
    }
    button {
      background: var(--btn-bg);
      color: var(--btn-fg);
      border: 1px solid transparent;
      border-radius: 4px;
      padding: 6px 12px;
      cursor: pointer;
      font-size: 12px;
      font-family: inherit;
    }
    button:hover { background: var(--btn-hover); }
    button.primary { background: var(--accent); color: var(--accent-fg); }
    button.active {
      background: var(--accent);
      color: var(--accent-fg);
      border-color: var(--fg);
    }
    button.muted-ch {
      opacity: 0.42;
      text-decoration: line-through;
    }
    .stats {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
      margin-top: 10px;
      font-family: var(--vscode-editor-font-family, monospace);
      font-size: 12px;
    }
    .stat-box {
      padding: 6px 8px;
      border-radius: 4px;
      background: rgba(128,128,128,0.1);
    }
    .stat-label { color: var(--muted); font-size: 10px; display: block; }
    .seek-wrap {
      margin-top: 10px;
      display: flex;
      align-items: center;
      gap: 8px;
    }
    input[type="range"] {
      flex: 1;
      accent-color: var(--accent);
    }
    .meter-wrap {
      margin-top: 10px;
      height: 10px;
      background: rgba(128,128,128,0.15);
      border-radius: 5px;
      overflow: hidden;
    }
    .meter-bar {
      height: 100%;
      width: 0%;
      background: linear-gradient(90deg, #3fb950, #d29922, #f85149);
      transition: width 40ms linear;
    }
    .section-label {
      font-size: 11px;
      color: var(--muted);
      margin-bottom: 6px;
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }
    .status-msg {
      color: var(--muted);
      font-size: 11px;
      margin-top: 8px;
    }
  </style>
</head>
<body>
  <div class="header">
    <div>
      <div class="title" id="songTitle">No track loaded</div>
      <div class="subtitle">YM2203 (OPN: FM 3ch + SSG 3ch) • ymfm + mdxnt C99 driver (Wasm)</div>
    </div>
    <div class="subtitle" id="sizeLabel"></div>
  </div>

  <div class="card">
    <div class="row">
      <button id="btnPlayPause" class="primary">▶ Play</button>
      <button id="btnStop">■ Stop</button>
      <button id="btnRewind">⏪ -1 Bar</button>
      <button id="btnForward">⏩ +1 Bar</button>
    </div>

    <div class="seek-wrap">
      <span class="subtitle">Seek</span>
      <input type="range" id="seekSlider" min="0" max="3840" value="0" step="48" />
      <span class="subtitle" id="seekVal">0</span>
    </div>

    <div class="meter-wrap">
      <div class="meter-bar" id="meterBar"></div>
    </div>

    <div class="stats">
      <div class="stat-box"><span class="stat-label">TICK</span><span id="statTick">0</span></div>
      <div class="stat-box"><span class="stat-label">BAR : BEAT</span><span id="statBar">1 : 1</span></div>
      <div class="stat-box"><span class="stat-label">BPM (TB)</span><span id="statBpm">120 (220)</span></div>
    </div>
  </div>

  <div class="card">
    <div class="section-label">Playback Speed</div>
    <div class="row" id="speedButtons">
      <button data-speed="50">0.5x</button>
      <button data-speed="100" class="active">1x</button>
      <button data-speed="200">2x</button>
      <button data-speed="400">4x</button>
      <button data-speed="800">8x</button>
    </div>
  </div>

  <div class="card">
    <div class="section-label">Channels (Click: Mute / Shift+Click: Solo)</div>
    <div class="row" id="chButtons">
      <button data-ch="0">FM 1 (A)</button>
      <button data-ch="1">FM 2 (B)</button>
      <button data-ch="2">FM 3 (C)</button>
      <button data-ch="3">SSG A (D)</button>
      <button data-ch="4">SSG B (E)</button>
      <button data-ch="5">SSG C (F)</button>
    </div>
  </div>

  <div class="status-msg" id="statusMsg">Ready. Click ▶ in an .mml editor to compile &amp; play.</div>

  <script nonce="${nonce}">
    window.MDXNT_CONFIG = {
      workletUrl: ${JSON.stringify(workletJsUri.toString())},
      wasmUrl: ${JSON.stringify(wasmUri.toString())}
    };
  </script>
  <script nonce="${nonce}" src="${playerJsUri}"></script>
</body>
</html>`;
}

function ensurePlayerPanel(context: vscode.ExtensionContext): vscode.WebviewPanel {
  if (playerPanel) {
    playerPanel.reveal(vscode.ViewColumn.Beside, true);
    return playerPanel;
  }

  playerReady = false;
  playerPanel = vscode.window.createWebviewPanel(
    'mdxntPlayer',
    'mdxnt OPN Player',
    { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true },
    {
      enableScripts: true,
      retainContextWhenHidden: true,
      localResourceRoots: [vscode.Uri.joinPath(context.extensionUri, 'media')],
    }
  );

  playerPanel.webview.html = getWebviewHtml(playerPanel.webview, context);

  playerPanel.webview.onDidReceiveMessage((msg) => {
    if (msg.type === 'ready') {
      playerReady = true;
      if (pendingLoad && playerPanel) {
        playerPanel.webview.postMessage({
          type: 'load',
          name: pendingLoad.name,
          data: pendingLoad.data,
        });
        pendingLoad = undefined;
      }
    } else if (msg.type === 'speedChanged') {
      fastForwardActive = Number(msg.speed) > 100;
      updateStatusBar();
    } else if (msg.type === 'error') {
      vscode.window.showErrorMessage(`mdxnt player: ${msg.message}`);
    }
  });

  playerPanel.onDidDispose(() => {
    playerPanel = undefined;
    playerReady = false;
    pendingLoad = undefined;
    fastForwardActive = false;
    updateStatusBar();
  });

  return playerPanel;
}

function updateStatusBar() {
  if (!statusBarItem) return;
  const ff = fastForwardActive ? ' [4x]' : '';
  statusBarItem.text = `$(play) mdxnt${ff}`;
}

async function startLsp(context: vscode.ExtensionContext) {
  const lspBin = findBinary('lspPath', 'mdxnt-lsp', context);
  if (!lspBin) {
    return;
  }

  const serverOptions: ServerOptions = {
    command: lspBin,
    args: [],
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'mdxnt-mml' }],
  };

  const client = new LanguageClient(
    'mdxnt-lsp',
    'mdxnt MML Language Server',
    serverOptions,
    clientOptions
  );

  await client.start();
  lspClient = client;
}

export async function activate(context: vscode.ExtensionContext) {
  statusBarItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
  statusBarItem.command = 'mdxnt.compileAndPlay';
  statusBarItem.tooltip = 'MDXNT: Compile & Play current MML (Ctrl+Alt+P)';
  updateStatusBar();
  context.subscriptions.push(statusBarItem);

  const refreshStatusVisibility = () => {
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.languageId === 'mdxnt-mml') {
      statusBarItem?.show();
    } else {
      statusBarItem?.hide();
    }
  };
  refreshStatusVisibility();
  context.subscriptions.push(
    vscode.window.onDidChangeActiveTextEditor(refreshStatusVisibility)
  );

  // Start Language Server
  startLsp(context).catch((err) => {
    vscode.window.showWarningMessage(`mdxnt-lsp failed to start: ${err.message}`);
  });

  context.subscriptions.push(
    vscode.commands.registerCommand('mdxnt.openPlayer', () => {
      ensurePlayerPanel(context);
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('mdxnt.compileAndPlay', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== 'mdxnt-mml') {
        vscode.window.showInformationMessage('Open an .mml file to compile and play.');
        return;
      }

      try {
        const bin = await compileMmlText(editor.document.getText(), context);
        const name = path.basename(editor.document.fileName);
        const panel = ensurePlayerPanel(context);
        const payload = { name, data: Array.from(bin) };
        if (playerReady) {
          panel.webview.postMessage({ type: 'load', ...payload });
        } else {
          pendingLoad = payload;
        }
        fastForwardActive = false;
        updateStatusBar();
      } catch (err: unknown) {
        const msg = err instanceof Error ? err.message : String(err);
        vscode.window.showErrorMessage(`MML compile error:\n${msg}`);
      }
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('mdxnt.stop', () => {
      fastForwardActive = false;
      updateStatusBar();
      playerPanel?.webview.postMessage({ type: 'stop' });
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('mdxnt.fastForward', () => {
      if (!playerPanel || !playerReady) {
        return;
      }
      fastForwardActive = !fastForwardActive;
      updateStatusBar();
      playerPanel.webview.postMessage({
        type: 'setSpeed',
        speed: fastForwardActive ? 400 : 100,
      });
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('mdxnt.compile', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== 'mdxnt-mml') {
        vscode.window.showInformationMessage('Open an .mml file to compile.');
        return;
      }
      try {
        const bin = await compileMmlText(editor.document.getText(), context);
        const defaultOut = editor.document.fileName.replace(/\.mml$/i, '') + '.mdxt';
        const uri = await vscode.window.showSaveDialog({
          defaultUri: vscode.Uri.file(defaultOut),
          filters: { 'MDXT Binary': ['mdxt'] },
        });
        if (uri) {
          fs.writeFileSync(uri.fsPath, bin);
          vscode.window.showInformationMessage(
            `Compiled ${path.basename(uri.fsPath)} (${bin.length} B)`
          );
        }
      } catch (err: unknown) {
        const msg = err instanceof Error ? err.message : String(err);
        vscode.window.showErrorMessage(`MML compile error:\n${msg}`);
      }
    })
  );
}

export async function deactivate(): Promise<void> {
  playerPanel?.dispose();
  playerPanel = undefined;
  if (lspClient) {
    const client = lspClient;
    lspClient = undefined;
    await client.stop();
  }
}
