// Copyright (c) 2026 Matteo La Barbera. MIT License.
//
// Transport between the MCP server and the Unreal editor process that hosts the
// Blueprint Agent Bridge plugin. Three modes:
//   http       talk to an editor that already runs the bridge (editor started with -BABServer,
//              or the console command BAB.StartServer)
//   auto       like http, but when nothing answers it starts a headless commandlet
//              (UnrealEditor-Cmd -run=BlueprintAgentBridge -serve) and keeps it for the session
//   commandlet one process per request (slow, no state kept); useful for CI
import { spawn, execSync, type ChildProcess } from "node:child_process";
import { existsSync, readFileSync, readdirSync, mkdirSync, openSync, closeSync, writeFileSync, unlinkSync, statSync } from "node:fs";
import { join, dirname, resolve, basename } from "node:path";
import { tmpdir, platform } from "node:os";

export type BridgeMode = "auto" | "http" | "commandlet";

export interface BridgeConfig {
  mode: BridgeMode;
  host: string;
  port: number;
  projectFile?: string;
  engineDir?: string;
  startupTimeoutSec: number;
  commandletTimeoutSec: number;
  keepAlive: boolean;
  /** Start a headless editor even when an interactive editor already has the project open. */
  allowSecondEditor: boolean;
  extraArgs: string[];
}

export interface EditorProcess {
  pid: number;
  commandLine: string;
  /** true for a headless bridge (-run=BlueprintAgentBridge), false for an interactive editor */
  isBridge: boolean;
}

export interface BridgeResponse {
  ok: boolean;
  op?: string;
  result: Record<string, unknown>;
  errors: string[];
  warnings: string[];
  elapsed_ms?: number;
}

export interface HealthInfo {
  ok: boolean;
  engine_version?: string;
  project?: string;
  mode?: string;
  pid?: number;
  uptime_s?: number;
  requests?: number;
}

export function log(message: string): void {
  process.stderr.write(`[blueprint-agent-bridge] ${message}\n`);
}

// ---------------------------------------------------------------------------------------------
// Configuration

function argValue(argv: string[], name: string): string | undefined {
  const index = argv.indexOf(name);
  if (index >= 0 && index + 1 < argv.length) return argv[index + 1];
  const prefixed = argv.find((a) => a.startsWith(name + "="));
  return prefixed ? prefixed.slice(name.length + 1) : undefined;
}

export function findProjectFile(startDir: string): string | undefined {
  let dir = resolve(startDir);
  for (let depth = 0; depth < 12; depth++) {
    try {
      const candidates = readdirSync(dir).filter((f) => f.toLowerCase().endsWith(".uproject"));
      if (candidates.length === 1) return join(dir, candidates[0]);
      if (candidates.length > 1) return join(dir, candidates.sort()[0]);
    } catch {
      /* ignore unreadable directories */
    }
    const parent = dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return undefined;
}

export function loadConfig(argv: string[], env: NodeJS.ProcessEnv): BridgeConfig {
  const mode = (argValue(argv, "--mode") ?? env.BAB_MODE ?? "auto").toLowerCase() as BridgeMode;
  const projectArg = argValue(argv, "--project") ?? env.BAB_PROJECT;
  let projectFile = projectArg ? resolve(projectArg) : findProjectFile(process.cwd());
  if (projectFile && statSync(projectFile, { throwIfNoEntry: false })?.isDirectory()) {
    projectFile = findProjectFile(projectFile);
  }
  const engineDir = argValue(argv, "--engine") ?? env.BAB_ENGINE_DIR ?? findEngineDir(projectFile);
  return {
    mode: ["auto", "http", "commandlet"].includes(mode) ? mode : "auto",
    host: env.BAB_HOST ?? "127.0.0.1",
    port: Number(argValue(argv, "--port") ?? env.BAB_PORT ?? 8765),
    projectFile,
    engineDir,
    startupTimeoutSec: Number(argValue(argv, "--timeout") ?? env.BAB_STARTUP_TIMEOUT ?? 600),
    commandletTimeoutSec: Number(argValue(argv, "--commandlet-timeout") ?? env.BAB_COMMANDLET_TIMEOUT ?? 900),
    keepAlive: argv.includes("--keep-alive") || /^(1|true|yes)$/i.test(env.BAB_KEEP_ALIVE ?? ""),
    allowSecondEditor: argv.includes("--allow-second-editor") || /^(1|true|yes)$/i.test(env.BAB_ALLOW_SECOND_EDITOR ?? ""),
    extraArgs: (env.BAB_EXTRA_ARGS ?? "").split(" ").filter(Boolean),
  };
}

// ---------------------------------------------------------------------------------------------
// Running editors

function listUnrealProcesses(): Array<{ pid: number; commandLine: string }> {
  try {
    if (platform() === "win32") {
      const script =
        "Get-CimInstance Win32_Process -Filter \"Name='UnrealEditor.exe' OR Name='UnrealEditor-Cmd.exe'\" | ForEach-Object { '{0}|{1}' -f $_.ProcessId, $_.CommandLine }";
      const encoded = Buffer.from(script, "utf16le").toString("base64");
      const out = execSync(`powershell -NoProfile -NonInteractive -EncodedCommand ${encoded}`, { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], timeout: 15000, windowsHide: true });
      return out
        .split(/\r?\n/)
        .map((line) => line.trim())
        .filter(Boolean)
        .map((line) => {
          const sep = line.indexOf("|");
          return { pid: Number(line.slice(0, sep)), commandLine: line.slice(sep + 1) };
        })
        .filter((p) => Number.isFinite(p.pid) && p.pid > 0);
    }
    const out = execSync("ps -axo pid=,command=", { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], timeout: 15000 });
    return out
      .split("\n")
      .map((line) => line.trim())
      .filter((line) => /UnrealEditor/.test(line))
      .map((line) => {
        const match = line.match(/^(\d+)\s+(.*)$/);
        return match ? { pid: Number(match[1]), commandLine: match[2] } : { pid: 0, commandLine: line };
      })
      .filter((p) => p.pid > 0);
  } catch {
    return [];
  }
}

/** Unreal Editor processes that have this project open, other than the given pid. */
export function findEditorProcesses(projectFile: string | undefined, excludePid?: number): EditorProcess[] {
  if (!projectFile) return [];
  const normalized = resolve(projectFile).replace(/\\/g, "/").toLowerCase();
  const name = basename(projectFile).toLowerCase();
  return listUnrealProcesses()
    .filter((p) => p.pid !== excludePid && p.pid !== process.pid)
    .filter((p) => {
      const cmd = p.commandLine.replace(/\\/g, "/").toLowerCase();
      return cmd.includes(normalized) || cmd.includes("/" + name) || cmd.includes(" " + name);
    })
    .map((p) => ({ pid: p.pid, commandLine: p.commandLine, isBridge: /-run=blueprintagentbridge/i.test(p.commandLine) }));
}

export function describeEditorProcesses(editors: EditorProcess[]): string {
  if (editors.length === 0) return "no other Unreal Editor process has this project open";
  return editors
    .map((e) => `pid ${e.pid}: ${e.isBridge ? "headless bridge (-run=BlueprintAgentBridge)" : "interactive editor"}${/-babserver/i.test(e.commandLine) ? " started with -BABServer" : ""}`)
    .join("; ");
}

// ---------------------------------------------------------------------------------------------
// Engine discovery

export function editorCmdPath(engineDir: string): string {
  switch (platform()) {
    case "win32":
      return join(engineDir, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe");
    case "darwin":
      return join(engineDir, "Engine", "Binaries", "Mac", "UnrealEditor.app", "Contents", "MacOS", "UnrealEditor-Cmd");
    default:
      return join(engineDir, "Engine", "Binaries", "Linux", "UnrealEditor-Cmd");
  }
}

function isEngineDir(dir: string | undefined): dir is string {
  return !!dir && existsSync(editorCmdPath(dir));
}

function readEngineAssociation(projectFile?: string): string | undefined {
  if (!projectFile || !existsSync(projectFile)) return undefined;
  try {
    const json = JSON.parse(readFileSync(projectFile, "utf8"));
    return typeof json.EngineAssociation === "string" ? json.EngineAssociation : undefined;
  } catch {
    return undefined;
  }
}

function regQuery(key: string, value: string): string | undefined {
  try {
    const out = execSync(`reg query "${key}" /v "${value}"`, { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
    const match = out.match(/REG_SZ\s+(.+)$/m);
    return match ? match[1].trim() : undefined;
  } catch {
    return undefined;
  }
}

export function findEngineDir(projectFile?: string): string | undefined {
  const association = readEngineAssociation(projectFile);
  const candidates: string[] = [];

  if (platform() === "win32") {
    // Source builds registered by UnrealVersionSelector use a GUID association.
    if (association && /^\{?[0-9a-f-]{32,36}\}?$/i.test(association)) {
      const dir = regQuery("HKCU\\Software\\Epic Games\\Unreal Engine\\Builds", association);
      if (dir) candidates.push(dir);
    }
    if (association && !association.includes("{")) {
      const dir = regQuery(`HKLM\\SOFTWARE\\EpicGames\\Unreal Engine\\${association}`, "InstalledDirectory");
      if (dir) candidates.push(dir);
    }
    const launcherDat = join(process.env.ProgramData ?? "C:\\ProgramData", "Epic", "UnrealEngineLauncher", "LauncherInstalled.dat");
    if (existsSync(launcherDat)) {
      try {
        const list = JSON.parse(readFileSync(launcherDat, "utf8")).InstallationList as Array<{ AppName: string; InstallLocation: string }>;
        const wanted = association ? `UE_${association}` : undefined;
        const installs = list.filter((i) => /^UE_\d/.test(i.AppName));
        const exact = wanted ? installs.find((i) => i.AppName === wanted) : undefined;
        if (exact) candidates.push(exact.InstallLocation);
        for (const i of installs.sort((a, b) => b.AppName.localeCompare(a.AppName))) candidates.push(i.InstallLocation);
      } catch {
        /* ignore */
      }
    }
    if (association) candidates.push(join("C:\\Program Files\\Epic Games", `UE_${association}`));
  } else if (platform() === "darwin") {
    if (association) candidates.push(join("/Users/Shared/Epic Games", `UE_${association}`));
  }
  // A source-build project lives under <engine>/<Project>/...; walk up a few levels.
  if (projectFile) {
    let dir = dirname(projectFile);
    for (let i = 0; i < 4; i++) {
      dir = dirname(dir);
      candidates.push(dir);
    }
  }
  return candidates.find(isEngineDir);
}

// ---------------------------------------------------------------------------------------------
// Bridge

export class Bridge {
  private child?: ChildProcess;
  private spawnedByUs = false;
  private starting?: Promise<void>;

  constructor(public readonly config: BridgeConfig) {}

  get baseUrl(): string {
    return `http://${this.config.host}:${this.config.port}`;
  }

  get serverLogFile(): string | undefined {
    if (!this.config.projectFile) return undefined;
    return join(dirname(this.config.projectFile), "Saved", "Logs", "BlueprintAgentBridge-server.log");
  }

  describe(): string {
    return [
      `mode=${this.config.mode}`,
      `url=${this.baseUrl}`,
      `project=${this.config.projectFile ?? "(not found: set BAB_PROJECT or --project)"}`,
      `engine=${this.config.engineDir ?? "(not found: set BAB_ENGINE_DIR or --engine)"}`,
    ].join(" ");
  }

  async health(timeoutMs = 2000): Promise<HealthInfo | null> {
    try {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), timeoutMs);
      const response = await fetch(`${this.baseUrl}/health`, { signal: controller.signal });
      clearTimeout(timer);
      if (!response.ok) return null;
      return (await response.json()) as HealthInfo;
    } catch {
      return null;
    }
  }

  get expectedProjectName(): string | undefined {
    return this.config.projectFile ? basename(this.config.projectFile, ".uproject") : undefined;
  }

  /** The bridge that answers on the port must belong to the project this server was configured for. */
  private checkProjectMatches(info: HealthInfo): void {
    const expected = this.expectedProjectName;
    if (expected && info.project && info.project.toLowerCase() !== expected.toLowerCase()) {
      throw new Error(
        `The bridge on ${this.baseUrl} belongs to project "${info.project}" (pid ${info.pid}), not "${expected}". ` +
          `Use --port / BAB_PORT to pick a free port for this project, or stop that server first.`
      );
    }
  }

  async ensureConnected(): Promise<void> {
    if (this.config.mode === "commandlet") return;
    const info = await this.health();
    if (info) {
      this.checkProjectMatches(info);
      return;
    }
    const editors = findEditorProcesses(this.config.projectFile, this.child?.pid);
    const interactive = editors.filter((e) => !e.isBridge);
    const otherBridges = editors.filter((e) => e.isBridge);
    if (this.config.mode === "http") {
      throw new Error(
        `No Blueprint Agent Bridge server answers at ${this.baseUrl} (${describeEditorProcesses(editors)}). ` +
          `In the open editor run the console command "BAB.StartServer ${this.config.port}", or start it with -BABServer, ` +
          `or set BAB_MODE=auto to let this server launch a headless editor.`
      );
    }
    if (interactive.length > 0 && !this.config.allowSecondEditor) {
      throw new Error(
        `Unreal Editor already has this project open without the bridge server (${describeEditorProcesses(interactive)}). ` +
          `Starting a second, headless editor would edit a separate copy: your open editor would not show the changes and could overwrite them on save. ` +
          `Do one of these: (1) in that editor, open the console (~) and run "BAB.StartServer ${this.config.port}", then retry; ` +
          `(2) enable "Auto Start Server" in Editor Preferences > Plugins > Blueprint Agent Bridge so it is always on; ` +
          `(3) close the editor; (4) only if you really want two editors, start this MCP server with --allow-second-editor (or BAB_ALLOW_SECOND_EDITOR=1).`
      );
    }
    if (otherBridges.length > 0 && !this.config.allowSecondEditor) {
      throw new Error(
        `A headless bridge for this project is already running but not on port ${this.config.port} (${describeEditorProcesses(otherBridges)}). ` +
          `Point this server at its port with --port, stop it, or pass --allow-second-editor.`
      );
    }
    if (!this.starting) {
      this.starting = this.startServer().finally(() => {
        this.starting = undefined;
      });
    }
    await this.starting;
  }

  /** Other Unreal Editor processes that have the project open (for status reports). */
  detectEditors(): EditorProcess[] {
    return findEditorProcesses(this.config.projectFile, this.child?.pid);
  }

  private requireLaunchInfo(): { cmd: string; project: string } {
    if (!this.config.projectFile || !existsSync(this.config.projectFile)) {
      throw new Error("No .uproject found. Pass --project <path/to/Game.uproject> or set BAB_PROJECT (the project must have the BlueprintAgentBridge plugin enabled).");
    }
    if (!this.config.engineDir || !isEngineDir(this.config.engineDir)) {
      throw new Error("Unreal Engine installation not found. Set BAB_ENGINE_DIR (e.g. C:\\Program Files\\Epic Games\\UE_5.6) or pass --engine.");
    }
    return { cmd: editorCmdPath(this.config.engineDir), project: this.config.projectFile };
  }

  async startServer(): Promise<void> {
    const { cmd, project } = this.requireLaunchInfo();
    const logFile = this.serverLogFile!;
    mkdirSync(dirname(logFile), { recursive: true });
    const fd = openSync(logFile, "w");
    const args = [
      project,
      "-run=BlueprintAgentBridge",
      "-serve",
      `-port=${this.config.port}`,
      "-unattended",
      "-nosplash",
      "-nullrhi",
      "-NoLogTimes",
      "-stdout",
      "-FullStdOutLogOutput",
      ...this.config.extraArgs,
    ];
    log(`starting headless editor: ${basename(cmd)} ${args.join(" ")}`);
    log(`server log: ${logFile}`);
    this.child = spawn(cmd, args, { stdio: ["ignore", fd, fd], detached: this.config.keepAlive, windowsHide: true });
    closeSync(fd);
    this.spawnedByUs = true;
    const child = this.child;
    let exited: number | null = null;
    child.on("exit", (code) => {
      exited = code ?? -1;
    });
    if (this.config.keepAlive) child.unref();

    const deadline = Date.now() + this.config.startupTimeoutSec * 1000;
    while (Date.now() < deadline) {
      if (exited !== null) {
        this.child = undefined;
        this.spawnedByUs = false;
        throw new Error(`The headless editor exited with code ${exited} before the server was ready. Last log lines:\n${this.tailLog(40)}`);
      }
      const info = await this.health(1500);
      if (info?.ok) {
        log(`server ready (engine ${info.engine_version}, pid ${info.pid})`);
        return;
      }
      await new Promise((r) => setTimeout(r, 1000));
    }
    // Do not leave an orphaned editor behind.
    this.killChild();
    throw new Error(`Timed out after ${this.config.startupTimeoutSec}s waiting for the headless editor. Last log lines:\n${this.tailLog(40)}`);
  }

  private killChild(): void {
    const child = this.child;
    this.child = undefined;
    this.spawnedByUs = false;
    if (child && child.exitCode === null) {
      try {
        child.kill();
      } catch {
        /* ignore */
      }
    }
  }

  /** True when the bridge process answering on the port was started by this MCP server. */
  ownsServer(): boolean {
    return this.spawnedByUs && !!this.child && this.child.exitCode === null;
  }

  tailLog(lines: number): string {
    const file = this.serverLogFile;
    if (!file || !existsSync(file)) return "(no log file)";
    const content = readFileSync(file, "utf8").split(/\r?\n/).filter(Boolean);
    return content.slice(-lines).join("\n");
  }

  /**
   * Stops the bridge. Only a process started by this MCP server is stopped unless force is set:
   * an interactive editor (or a server started by another tool) belongs to the user.
   */
  async stopServer(force = false): Promise<"stopped" | "not-running" | "not-owned"> {
    const info = await this.health();
    if (!info) {
      this.killChild();
      return "not-running";
    }
    if (!this.spawnedByUs && !force) {
      return "not-owned";
    }
    try {
      await fetch(`${this.baseUrl}/shutdown`, { method: "POST" });
    } catch {
      /* the process may close the socket while answering */
    }
    const child = this.child;
    if (child && this.spawnedByUs) {
      const deadline = Date.now() + 30000;
      while (Date.now() < deadline && child.exitCode === null) {
        await new Promise((r) => setTimeout(r, 250));
      }
      this.killChild();
    }
    return "stopped";
  }

  async dispose(): Promise<void> {
    if (this.spawnedByUs && !this.config.keepAlive) {
      log("stopping the headless editor");
      await this.stopServer();
    }
  }

  describeOwnership(): string {
    return this.ownsServer() ? "started by this MCP server" : "started elsewhere (interactive editor or another tool)";
  }

  async call(op: string, params: Record<string, unknown>): Promise<BridgeResponse> {
    const request = { op, params };
    if (this.config.mode === "commandlet") {
      return this.callCommandlet(request);
    }
    await this.ensureConnected();
    return this.post(request);
  }

  async batch(operations: Array<{ op: string; params: Record<string, unknown> }>, stopOnError = true): Promise<Record<string, unknown>> {
    const request = { batch: operations, stop_on_error: stopOnError };
    if (this.config.mode === "commandlet") {
      return this.callCommandlet(request) as unknown as Record<string, unknown>;
    }
    await this.ensureConnected();
    return this.post(request) as unknown as Record<string, unknown>;
  }

  private async post(body: unknown): Promise<BridgeResponse> {
    const response = await fetch(`${this.baseUrl}/rpc`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
    const text = await response.text();
    try {
      return JSON.parse(text) as BridgeResponse;
    } catch {
      return { ok: false, result: {}, errors: [`Invalid response from the bridge (${response.status}): ${text.slice(0, 500)}`], warnings: [] };
    }
  }

  private async callCommandlet(request: unknown): Promise<BridgeResponse> {
    const { cmd, project } = this.requireLaunchInfo();
    const stamp = `${Date.now()}-${process.pid}`;
    const requestFile = join(tmpdir(), `bab-request-${stamp}.json`);
    const responseFile = join(tmpdir(), `bab-response-${stamp}.json`);
    writeFileSync(requestFile, JSON.stringify(request), "utf8");
    const args = [project, "-run=BlueprintAgentBridge", `-request=${requestFile}`, `-response=${responseFile}`, "-unattended", "-nosplash", "-nullrhi", ...this.config.extraArgs];
    log(`running commandlet: ${basename(cmd)} ${args.join(" ")}`);
    const timeoutMs = this.config.commandletTimeoutSec * 1000;
    const outcome = await new Promise<{ code: number | null; error?: string; timedOut: boolean }>((resolvePromise) => {
      const child = spawn(cmd, args, { stdio: "ignore", windowsHide: true });
      const timer = setTimeout(() => {
        try {
          child.kill();
        } catch {
          /* ignore */
        }
        resolvePromise({ code: null, timedOut: true });
      }, timeoutMs);
      child.on("exit", (code) => {
        clearTimeout(timer);
        resolvePromise({ code, timedOut: false });
      });
      child.on("error", (error) => {
        clearTimeout(timer);
        resolvePromise({ code: null, error: error.message, timedOut: false });
      });
    });
    try {
      if (outcome.timedOut) {
        return { ok: false, result: {}, errors: [`The commandlet did not finish within ${this.config.commandletTimeoutSec}s and was terminated.`], warnings: [] };
      }
      if (outcome.error) {
        return { ok: false, result: {}, errors: [`Could not start ${cmd}: ${outcome.error}`], warnings: [] };
      }
      if (!existsSync(responseFile)) {
        return { ok: false, result: {}, errors: [`The commandlet exited with code ${outcome.code} and produced no response file (check the editor log in Saved/Logs).`], warnings: [] };
      }
      const response = JSON.parse(readFileSync(responseFile, "utf8")) as BridgeResponse;
      if (outcome.code !== 0 && outcome.code !== 2 && response.ok) {
        response.warnings = [...(response.warnings ?? []), `The commandlet exited with code ${outcome.code}.`];
      }
      return response;
    } finally {
      for (const f of [requestFile, responseFile]) {
        try {
          unlinkSync(f);
        } catch {
          /* ignore */
        }
      }
    }
  }
}
