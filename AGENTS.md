# Instructions for coding agents working in this repository

This repository builds the Blueprint Agent Bridge: an Unreal Engine 5.6 editor plugin (C++) and an
MCP server (TypeScript) that let agents edit Blueprints. Project conventions and roadmap are in
`CLAUDE.md` (Italian). Technical references: `docs/architecture.md`, `docs/protocol.md`,
`docs/graph-format.md`.

- Plugin source: `Plugin/BlueprintAgentBridge/Source/BlueprintAgentBridge/`. Build with
  `scripts/build.ps1` (or `Engine/Build/BatchFiles/Build.bat BPBridgeTestEditor Win64 Development -Project=test-project/BPBridgeTest.uproject`).
- Tests: `scripts/test.ps1` runs the Unreal automation tests `BlueprintAgentBridge.*` headless.
  `node mcp-server/dist/selftest.js --project test-project/BPBridgeTest.uproject` exercises the whole stack.
- Verify engine APIs against `Engine/Source` of the installed UE version before using them; do not
  rely on memory. Document any reverse engineering in `docs/reverse-engineering/`.
- Keep every operation registered in `FBABDispatcher` returning `FBABResult` with clear
  `errors[]`; never assert on user input.

When you are an agent *using* the bridge to edit Blueprints (not developing it), follow
`skills/blueprint-agent-bridge/SKILL.md`.
