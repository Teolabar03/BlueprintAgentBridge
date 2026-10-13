# Changelog

## 0.2.1 (2026-10-13)

Security and data-safety fixes from a code review.

- HTTP server: random per-start token (`X-BAB-Token`, stored in
  `Saved/BlueprintAgentBridge/server-<port>.token`) required on `/rpc` and `/shutdown`; requests
  with an `Origin` header or a non-loopback `Host` are refused, so web pages cannot reach the editor
  (CSRF, DNS rebinding). The MCP server sends the token automatically.
- Writes are limited to files inside the project directory (no engine content).
- `bp_save` backs up the previous asset file to `Saved/BlueprintAgentBridge/Backups` (last 10 per
  asset) and reports it as `backup`.
- `-serve -parentpid=<pid>`: the headless editor exits when the MCP server that started it dies
  (passed automatically unless `--keep-alive`).
- The engine registry lookup runs `reg` without a shell.
- A busy port is now detected in commandlet mode: before, the bind failed silently inside
  `StartAllListeners`, the second server reported itself as listening and kept running.
- Pins destroyed or reordered by the engine are no longer dereferenced: autowiring into a wildcard
  (Select option), `bp_disconnect_pins` on split pins, `bp_recombine_pin` given a sub pin, and
  `bp_connect_pins` when callbacks rebuild pins. `bp_add_pin` reports the new pin by name (on a
  Select it used to report `ReturnValue`).
- The MCP server runs bridge requests one at a time in call order, including `bp_server`
  start/stop/restart; a restart waits until the old server has really stopped.
- Failed edits are really undone. `FScopedTransaction::Cancel` only drops the undo record and keeps
  the changes, so since 0.1.1 a failed edit could leave a half-built graph; the transaction is now
  closed and undone without a redo entry.
- The headless server creates the editor transaction buffer (commandlets do not get one), so the
  rollback above and `bp_undo` / `bp_redo` work there too.
- The token file is created with an ACL for the current user and SYSTEM only (mode 0600 elsewhere).
- The project-directory check resolves symbolic links and junctions, so a link inside `Content`
  cannot lead a save outside the project.

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
