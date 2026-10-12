# Changelog

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
