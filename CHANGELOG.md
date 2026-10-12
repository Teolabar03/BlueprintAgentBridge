# Changelog

## 0.2.0 (2026-10-12)

Building whole actors.

- Components: `bp_add_component`, `bp_get_component`, `bp_set_component_property`,
  `bp_remove_component` (SimpleConstructionScript nodes, attachment, template properties).
- Component bound events (`component_event`), event dispatchers (`bp_add_event_dispatcher` and the
  `call_dispatcher` / `bind_dispatcher` / `unbind_dispatcher` / `clear_dispatcher` nodes).
- Interfaces: `bp_add_interface`, `bp_remove_interface`; interface functions resolve as events.
- Class defaults: `bp_get_class_defaults`, `bp_set_class_default`.
- Timelines: `bp_get_timeline`, `bp_update_timeline` with float, vector, linear color and event
  tracks (keys and interpolation); the timeline node is rebuilt with one pin per track.
- Positional struct literals (`[x, y, z]`) for any reflected property, reflected property setter
  shared by nodes, component templates and class default objects.

## 0.1.1 (2026-10-12)

Working safely with the editor open.

- Editor sessions: every edit is an undo transaction (`Blueprint Agent Bridge: <op>`), failed edits
  are rolled back, new `bp_undo` / `bp_redo` tools (limited to bridge transactions unless forced),
  warning while Play In Editor runs.
- The MCP server refuses to start a second headless editor when the project is already open in an
  interactive editor without the bridge (`--allow-second-editor` / `BAB_ALLOW_SECOND_EDITOR=1`
  overrides), explains how to turn the bridge on, and refuses a bridge that belongs to another
  project. `bp_server status` lists the editor processes that have the project open.

## 0.1.0 (2026-10-12)

First public version.

- Editor plugin for Unreal Engine 5.6 with 31 operations behind one JSON dispatcher:
  assets (list, create, describe, compile, save), graphs (read as text/JSON, search nodes,
  add/remove/update nodes, connect/disconnect pins, pin defaults, dynamic pins, split/recombine,
  reflected properties, copy/paste text, auto layout), members (variables, local variables,
  functions, graph removal).
- Headless commandlet (`-run=BlueprintAgentBridge -serve` or `-request/-response`) and in-editor
  HTTP server (`-BABServer`, `BAB.StartServer`, editor preference), loopback only.
- MCP server (TypeScript, stdio) that starts the headless editor on demand, with one tool per
  operation plus `bp_batch` and `bp_server`.
- Skill for agents, documentation of the text format and the protocol, automation tests and a
  self test.
- `examples/BridgeDemo`: a sample project whose Blueprints (`BP_RotatingPickup`, `BP_Door`) are
  built by a scripted MCP client (`mcp-server/src/demo.ts`), saved, and reloaded in a fresh
  headless editor; verified in commandlet, headless-server and in-editor modes.
- Variable defaults are read from and written to the class default object once a Blueprint has
  been compiled (the engine clears the staging string on the first full compile).
- Template events of new Blueprints are enabled instead of duplicated; connected template nodes
  are enabled automatically; `then (Completed)` labels on latent calls.
