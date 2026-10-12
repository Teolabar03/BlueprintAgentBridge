# Blueprint Agent Bridge

Let AI coding agents (Claude Code, Codex, any MCP client) **read and edit Unreal Engine
Blueprints without the editor UI**: add and remove nodes, connect pins, set defaults, create
variables and functions, compile and read the errors back, save.

It is an Unreal Engine **C++ editor plugin** plus an **MCP server**. The plugin uses the same
editor APIs the Blueprint editor uses (`UEdGraphSchema_K2`, `UK2Node_*`, `FBlueprintEditorUtils`,
`FKismetEditorUtilities`, `FBlueprintActionDatabase`), so everything it produces is a normal,
compiled, saveable Blueprint. It never touches `.uasset` bytes.

- Works **headless** (`UnrealEditor-Cmd -run=BlueprintAgentBridge -serve`, no window, NullRHI)
  or inside a **running editor** (edits show up live in open graphs).
- A **compact text format** with stable node ids that models read well, plus JSON for tools.
- **Node search** backed by the editor action database: "print string", "set actor location",
  "for each loop" return specs you can place directly.
- **Compile errors mapped to node ids**, destructive operations that report their impact,
  no writes without an explicit save.

Tested with **Unreal Engine 5.6.1** on Windows.

## Quick start

### 1. Install the plugin

Copy `Plugin/BlueprintAgentBridge` into your project `Plugins/` folder (or reference it with
`"AdditionalPluginDirectories"` in the `.uproject`, like `test-project/BPBridgeTest.uproject` does),
enable it in the `.uproject`:

```json
"Plugins": [ { "Name": "BlueprintAgentBridge", "Enabled": true } ]
```

and build the editor target once (the project needs a C++ target; a Blueprint-only project gets one
by adding any C++ class):

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" MyProjectEditor Win64 Development -Project="C:\path\MyProject.uproject" -WaitMutex
```

### 2. Install the MCP server

```bash
cd mcp-server
npm install
npm run build
```

### 3. Register it with your agent

Claude Code (one line, from your Unreal project folder so the `.uproject` is found automatically):

```bash
claude mcp add blueprint-agent-bridge -- node /path/to/BlueprintAgentBridge/mcp-server/dist/index.js --project "C:/path/MyProject.uproject"
```

Codex (`~/.codex/config.toml`):

```toml
[mcp_servers.blueprint_agent_bridge]
command = "node"
args = ["C:/path/BlueprintAgentBridge/mcp-server/dist/index.js", "--project", "C:/path/MyProject.uproject"]
```

Any client: run `node mcp-server/dist/index.js` over stdio. Configuration by flags or environment:

| flag | env | meaning |
|---|---|---|
| `--project <file.uproject>` | `BAB_PROJECT` | project to open (default: the first `.uproject` found walking up from the current directory) |
| `--engine <dir>` | `BAB_ENGINE_DIR` | engine root (default: resolved from the project `EngineAssociation` through the launcher registry) |
| `--mode auto\|http\|commandlet` | `BAB_MODE` | `auto` (default) connects to a running bridge or starts a headless editor; `http` only connects; `commandlet` runs one process per call |
| `--port <n>` | `BAB_PORT` | HTTP port, default 8765 |
| `--keep-alive` | `BAB_KEEP_ALIVE` | leave the headless editor running when the MCP server exits |
| `--timeout <s>` | `BAB_STARTUP_TIMEOUT` | how long to wait for the headless editor (default 600) |
| `--commandlet-timeout <s>` | `BAB_COMMANDLET_TIMEOUT` | per-request limit in `commandlet` mode (default 900) |
| `--allow-second-editor` | `BAB_ALLOW_SECOND_EDITOR` | start a headless editor even if an interactive editor has the project open (see below) |

Then give the agent the skill in `skills/blueprint-agent-bridge/SKILL.md` (copy it to
`.claude/skills/` for Claude Code, or paste the content into `AGENTS.md` for Codex).

### 4. Try it

Ask the agent: *"In /Game/BP_Door, print 'Door ready' two seconds after BeginPlay, compile and save."*
It will call roughly: `bp_add_node event BeginPlay` → `bp_add_node call_function Delay` (autowired)
→ `bp_set_pin_default Duration 2` → `bp_add_node call_function PrintString` → `bp_set_pin_default
InString` → `bp_compile` → `bp_save`.

Without an agent, the self test does the same against the test project:

```bash
node mcp-server/dist/selftest.js --project test-project/BPBridgeTest.uproject
```

A longer scripted session drives the real MCP server (stdio, like Claude Code does) and builds
two gameplay Blueprints in [`examples/BridgeDemo`](examples/BridgeDemo/README.md), saves them and
reloads them in a fresh editor:

```bash
node mcp-server/dist/demo.js --project examples/BridgeDemo/BridgeDemo.uproject
```

## Working with the editor open

Start the editor with `-BABServer` (optionally `-BABPort=8765`), or enable *Auto Start Server* in
Editor Preferences > Plugins > Blueprint Agent Bridge (recommended: do it once per project), or type
`BAB.StartServer` in the console. The MCP server finds the running bridge first and connects to it:
edits happen inside your editor, open graphs update live, the asset turns dirty, `bp_compile` is the
Compile button and `bp_save` saves from the same process. Every editing tool is one step in the
editor undo history (Ctrl+Z works; the agent also has `bp_undo` / `bp_redo`, limited to its own
transactions unless forced).

If the editor is open **without** the bridge, the MCP server refuses to start a second, headless
editor and explains how to turn the bridge on. A second editor would edit a separate copy of the
assets: your open editor would not show the changes and could silently overwrite them on save
(Unreal does not warn when a loaded package changes on disk). `--allow-second-editor`
(`BAB_ALLOW_SECOND_EDITOR=1`) overrides the check if you really want that. The server also refuses
to use a bridge that belongs to a different project than the one it was configured for.

Avoid editing while Play In Editor is running; the bridge warns when it is.

## Tools

`bp_status`, `bp_list_assets`, `bp_create_blueprint`, `bp_describe_blueprint`, `bp_read_graph`,
`bp_get_node`, `bp_search_nodes`, `bp_add_node`, `bp_remove_node`, `bp_update_node`,
`bp_connect_pins`, `bp_disconnect_pins`, `bp_set_pin_default`, `bp_add_pin`, `bp_remove_pin`,
`bp_split_pin`, `bp_recombine_pin`, `bp_set_node_property`, `bp_reconstruct_node`,
`bp_export_text`, `bp_import_text`, `bp_auto_layout`, `bp_add_variable`, `bp_update_variable`,
`bp_remove_variable`, `bp_add_local_variable`, `bp_add_function`, `bp_remove_graph`,
`bp_add_component`, `bp_get_component`, `bp_set_component_property`, `bp_remove_component`,
`bp_get_class_defaults`, `bp_set_class_default`, `bp_add_interface`, `bp_remove_interface`,
`bp_add_event_dispatcher`, `bp_get_timeline`, `bp_update_timeline`, `bp_compile`, `bp_save`,
`bp_undo`, `bp_redo`, `bp_batch`, `bp_server`.

Full reference: [docs/protocol.md](docs/protocol.md). Text format: [docs/graph-format.md](docs/graph-format.md).
Design: [docs/architecture.md](docs/architecture.md).

## What the graph looks like to the agent

```
[A3F9C2D1] Event BeginPlay | event Actor.ReceiveBeginPlay @ 0,0
  > then : exec -> 7B2E4F10.execute
[7B2E4F10] Delay | call KismetSystemLibrary.Delay (latent) @ 420,0
  < execute : exec <- A3F9C2D1.then
  < Duration : float = 2.0
  > Completed : exec -> C01D9E77.execute
[C01D9E77] Print String | call KismetSystemLibrary.PrintString @ 840,0
  < execute : exec <- 7B2E4F10.Completed
  < InString : string = "Door ready"
  > then : exec
```

## Development

```powershell
# build plugin + test project
scripts\build.ps1
# run the automation tests headless (BlueprintAgentBridge.*)
scripts\test.ps1
# start the headless server by hand
scripts\serve.ps1 -Port 8765
```

The HTTP endpoint can be exercised with curl:

```bash
curl -s -X POST http://127.0.0.1:8765/rpc -d '{"op":"bp_read_graph","params":{"blueprint":"/Game/BP_Door"}}'
```

## Safety

- Nothing is written to disk until `bp_save` (or `save: true`). Work on a version-controlled project.
- `bp_save` compiles first and refuses to write a Blueprint with compile errors unless forced.
- Read-only (checked-in) and locked asset files are detected and reported before saving.
- Removing nodes, variables or graphs reports what breaks; removing something still in use needs `force`.
- The HTTP server listens on the loopback interface only.

## Status

Beta. Event graphs, functions, macros from StandardMacros, variables, components (add, read,
edit, remove), component events, event dispatchers, interfaces, class defaults, casts, struct
nodes, switches, timelines with float/vector/color/event tracks. Not covered yet: UMG designer,
animation graphs, Level Blueprints, user-defined struct/enum assets. See
[docs/architecture.md](docs/architecture.md#limitations-and-next-steps).

## License

MIT. Unreal Engine is a trademark of Epic Games, Inc.; this project is not affiliated with Epic Games.
