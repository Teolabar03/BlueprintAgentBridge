# Architecture

```
 agent (Claude Code, Codex, any MCP client)
   |  stdio (MCP)
   v
 mcp-server/  (TypeScript)  --- HTTP 127.0.0.1:8765 --->  Unreal Editor process
   |  spawns when needed                                   |
   +-- UnrealEditor-Cmd -run=BlueprintAgentBridge -serve   |   Plugin/BlueprintAgentBridge (C++)
                                                           |     FBABHttpServer   ----+
                                                           |     Commandlet       ----+--> FBABDispatcher --> operations
                                                           |     Automation tests ----+        |
                                                           |                                   v
                                                           |                 K2 editor API (UEdGraphSchema_K2, UK2Node_*,
                                                           |                 FBlueprintEditorUtils, FKismetEditorUtilities,
                                                           |                 FBlueprintActionDatabase)
```

## Decisions

**Edit through the engine, never the `.uasset` bytes.** The binary format changes between
versions, nodes and pins are identified by GUIDs, and an asset edited without recompiling is
corrupt in practice. The plugin uses exactly the code paths the editor UI uses: node creation via
`FGraphNodeCreator` / `UBlueprintNodeSpawner`, connections via `UEdGraphSchema_K2::TryCreateConnection`
(so automatic conversion nodes and type promotion behave as in the editor), pin defaults via
`TrySetDefaultValue` after `IsPinDefaultValid`, compilation via `FKismetEditorUtilities::CompileBlueprint`
with an `FCompilerResultsLog`, saving via `UPackage::SavePackage`.

**One dispatcher, several transports.** Every operation is a function
`FBABResult(const FJsonObject& Params)` registered by name in `FBABDispatcher`. The HTTP server,
the commandlet, the automation tests and the MCP server all go through it, so behaviour and
error messages are identical everywhere.

**Two execution modes, same plugin.**

| Mode | When | Notes |
|---|---|---|
| Headless commandlet (`-run=BlueprintAgentBridge -serve`) | default for agents; CI | No Slate, NullRHI, starts in 20-60 s. The commandlet ticks the core ticker, the task graph and the editor tickables so the HTTP module, asset loading and the action database keep working. |
| Interactive editor (`-BABServer`, console `BAB.StartServer`, or the setting) | when a human watches the editor | Graph edits appear live in open Blueprint editors because the same `UEdGraph` notifications fire. Every editing operation runs inside an `FScopedTransaction` named `Blueprint Agent Bridge: <op>`, so it is one undo step; a failed operation is undone (closing the transaction and undoing it, because `FScopedTransaction::Cancel` keeps the changes) and leaves nothing behind. The headless server creates the same transaction buffer, which a commandlet does not get by default. `bp_undo` / `bp_redo` act only on those transactions unless forced. |

**One editor per project.** Before starting a headless editor, the MCP server lists the Unreal
Editor processes that have the project open (PowerShell `Win32_Process` on Windows, `ps` elsewhere).
If an interactive editor without the bridge is found it refuses, because two processes would hold two
copies of the assets and the editor does not notice when a loaded package changes on disk (there is no
prompt; last writer wins). The health endpoint also reports the project name, and the server refuses to
talk to a bridge that belongs to another project.
| One-shot commandlet (`-request` / `-response`) | scripts, CI | Each call pays the engine startup. |

**Text format designed for agents.** See graph-format.md. Stable ids from node GUIDs, pins by
name, hidden pins omitted, compiler messages attached to nodes.

**Search backed by the editor action database.** `bp_search_nodes` scores every spawner in
`FBlueprintActionDatabase` with a cheap text match (function name, display name, class, keywords,
category, node class, spawner signature), then applies `FBlueprintActionFilter` with the Blueprint and
graph as context so results are actually placeable, and finally primes the UI spec only for the
returned items. Each result carries an `add_node_spec` that `bp_add_node` accepts directly, and a
`signature` that can spawn any action the editor menu can.

## Source layout

```
Plugin/BlueprintAgentBridge/
  BlueprintAgentBridge.uplugin
  Source/BlueprintAgentBridge/
    BlueprintAgentBridge.Build.cs
    Public/BlueprintAgentBridgeModule.h
    Private/
      BlueprintAgentBridgeModule.cpp  module startup, console commands, auto start in the editor
      BABTypes.*                      FBABResult + JSON helpers
      BABDispatcher.*                 operation registry, single/batch request handling
      BABContext.*                    resolving blueprints, graphs, nodes, pins, classes, functions, pin types, defaults
      BABGraphSerializer.*            graph/node/pin -> JSON and text; Blueprint summary
      BABNodeFactory.*                node creation from specs; reflected property setter
      BABNodeSearch.*                 action database search
      BABAutoLayout.*                 layered layout
      BABOperations*.cpp              the operations (assets/compile/save, graph/nodes/pins, variables/functions)
      BABHttpServer.*                 loopback HTTP endpoint (HTTPServer module)
      BABCommandlet.*                 -run=BlueprintAgentBridge
      BABSettings.h                   editor preferences
      Tests/BABAutomationTests.cpp    automation tests (BlueprintAgentBridge.*)
mcp-server/        TypeScript MCP server (stdio) + selftest
skills/            SKILL.md for Claude Code / instructions for Codex
test-project/      minimal UE 5.6 project that enables the plugin from ../Plugin
docs/
```

## Engine version

Developed and tested against **Unreal Engine 5.6.1** (launcher build). All engine calls were
checked against the 5.6 headers in `Engine/Source`; no reverse engineering was required (the source
shipped with the launcher build answered every question, see `docs/reverse-engineering/README.md`).
Known version-sensitive spots: `FKismetEditorUtilities::CompileBlueprint` options, the
`EMessageSeverity` enum, `UEdGraphSchema_K2::GetPinDefaultValuesFromString`, and the HTTPServer module
API. Porting to 5.4/5.5 should mostly be a matter of these.

## Limitations and next steps

- Timeline tracks are written as curves owned by the generated class (like the editor does);
  external curve assets can be referenced only through `bp_set_node_property` for now.
- Components are the Blueprint's own SimpleConstructionScript nodes; components inherited from a
  C++ parent are visible to graphs but not listed or editable.
- Widget (UMG) and Animation Blueprints load and their event graphs can be edited, but the
  designer/anim graphs are out of scope. User-defined struct and enum assets cannot be created yet.
- Undo history lives in memory only and disappears with the process: operate on a
  version-controlled project. `bp_save` keeps the previous file in `Saved/BlueprintAgentBridge/Backups`.
  The MCP skill tells the agent to never save without an explicit request.
- Level Blueprints are not supported (they live inside maps).
