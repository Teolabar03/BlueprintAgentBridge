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
