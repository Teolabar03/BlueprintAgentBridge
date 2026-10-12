# Phase 0: existing projects (October 2026)

Survey done before writing any code, to decide what to reuse. Claims come from each project's
README and the search results listed at the bottom; they were not all verified by running the code.

| Project | Language / transport | Blueprint graph support | Notes |
|---|---|---|---|
| [chongdashu/unreal-mcp](https://github.com/chongdashu/unreal-mcp) | C++ plugin + TCP socket, Python MCP server | event nodes, function calls, variables, components, self reference | UE 5.5+, Python 3.12+. Marked experimental by the author. Requires the interactive editor. |
| [flopperam/unreal-engine-mcp](https://github.com/flopperam/unreal-engine-mcp) | Python only (Python remote execution) | 23+ node types | Now owned by a commercial product (Aura). No C++ plugin, so limited to what the Python API exposes. |
| [sam-david/unreal-mcp](https://github.com/sam-david/unreal-mcp) | Python + Remote Control plugin, optional C++ plugin | add/connect/remove nodes only with the optional C++ plugin | 127 tools across editor subsystems; Blueprint graph editing is a small part. |
| [Hal-cell/unreal-blueprint-mcp](https://github.com/Hal-cell/unreal-blueprint-mcp) | C++ plugin (UE 5.4) + MCP server | branches, casts, macros, switches, arrays, struct make/break; "anchor-name" system to name nodes | Closest in spirit; inspired by chongdashu/flopperam. Interactive editor. |
| [cutehusky/ue5-mcp](https://github.com/cutehusky/ue5-mcp) | Python | function calls, events, variables, math, cast, struct nodes, connect pins, defaults | Started from the editor Windows menu. |
| [SallahBoussettah/UnrealMCP](https://github.com/SallahBoussettah/UnrealMCP) | hybrid Python + C++ | claims 45 node types, animation state machines, debugging | Self-reported; broad scope. |
| [remiphilippe/mcp-unreal](https://github.com/remiphilippe/mcp-unreal) | Go binary | Blueprint editing, headless builds and tests, UE API docs lookup | UE 5.7. |
| [gimmeDG/UnrealEngine5-mcp](https://github.com/gimmeDG/UnrealEngine5-mcp) | Python with RAG over the UE Python API | node graphs, GameplayAbilities | UE 5.6+. |
| [GenOrca/unreal-mcp](https://github.com/GenOrca/unreal-mcp) | Python / C++ | add/connect nodes, auto-layout, compile; Behavior Trees | |
| [tumourlove/unreal-blueprint-mcp](https://lobehub.com/mcp/tumourlove-unreal-blueprint-mcp) | C++ plugin serializing graphs to JSON | **read-only** graph reader | Useful as a reference for a JSON graph format. |
| [voodoofox/unreal-mcp](https://github.com/voodoofox/unreal-mcp) | Python + plugin | 16 graph editing tools out of 88 | Fork lineage of chongdashu. |

## Observations

1. **Everyone edits through the editor**, nobody patches `.uasset` bytes. That confirms the
   architectural decision in CLAUDE.md.
2. **Transport**: Python remote execution (flopperam, sam-david) or a custom TCP socket with
   hand-written JSON framing (chongdashu and its forks). None uses the engine's own `HTTPServer`
   module, and none can run **headless** (commandlet / `-nullrhi`): they all need an interactive
   editor, which makes CI and long agent sessions awkward.
3. **Graph representation**: mostly ad-hoc JSON dumps of nodes and pins, often with the editor
   GUIDs as ids or with user-chosen names ("anchors"). None documents a compact, round-trippable
   text format aimed at the model, and compile errors are rarely mapped back to node ids.
4. **Node creation**: all projects hard-code a handful of `UK2Node_*` classes. None uses
   `FBlueprintActionDatabase` for search + spawn, so "find me the node for X" is not possible
   beyond the hard-coded list.
5. **Known pitfalls reported by those projects** (and handled here): creating `UK2Node_Event`
   requires `EventReference.SetExternalMember` + `bOverrideFunction` and events must be unique;
   `UK2Node_CallFunction::SetFromFunction` must run before `AllocateDefaultPins`; variable nodes need
   the property from the **skeleton** class, which only exists after
   `MarkBlueprintAsStructurallyModified`; pin defaults must be validated with the schema or the
   compile fails later; after any structural change the Blueprint must be compiled before the
   generated class is usable.

## What was reused

Nothing was copied. The projects above confirmed the approach and the list of node kinds agents
ask for most (events, function calls, variables, branch, sequence, loops, casts, spawn, struct
make/break, switch). The differentiators of this project are: headless operation, the engine
`HTTPServer` module, a documented text format with stable ids, action-database search with
ready-to-use specs, and compile errors mapped to node ids.

Sources: GitHub search results and project READMEs as of 2026-10-12 (see the links in the table).
