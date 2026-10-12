# blueprint-agent-bridge-mcp

MCP server (stdio) for the Blueprint Agent Bridge Unreal Engine plugin. See the
[repository README](../README.md) for the full setup.

```bash
npm install
npm run build
node dist/index.js --project C:/path/MyProject.uproject
```

Modes (`--mode` / `BAB_MODE`):

- `auto` (default): use a running bridge on `127.0.0.1:8765` if there is one, otherwise start
  `UnrealEditor-Cmd <project> -run=BlueprintAgentBridge -serve` and keep it for the session.
  The engine is located from the project `EngineAssociation` (launcher registry,
  `LauncherInstalled.dat`, `C:\Program Files\Epic Games\UE_x.y`), or from `--engine` / `BAB_ENGINE_DIR`.
- `http`: only connect to an existing bridge (editor started with `-BABServer`).
- `commandlet`: one editor process per request. Slow, but needs no long-lived process.

Self test (creates a Blueprint in `/Game/BABTests`, builds BeginPlay -> Delay -> PrintString,
compiles it, prints the graph; nothing is saved):

```bash
node dist/selftest.js --project ../test-project/BPBridgeTest.uproject
```

Logs go to stderr; the headless editor log is written to `<project>/Saved/Logs/BlueprintAgentBridge-server.log`.
