# BPBridgeTest

Minimal Unreal Engine 5.6 project used to build and test the plugin. It enables
`BlueprintAgentBridge` from `../Plugin` through `AdditionalPluginDirectories`, so the plugin
source stays in one place.

- Build: `scripts\build.ps1` from the repository root.
- Tests: `scripts\test.ps1` (automation tests `BlueprintAgentBridge.*`), or
  `node mcp-server/dist/selftest.js --project test-project/BPBridgeTest.uproject`.
- Blueprints created by tests live in `/Game/BABTests` in memory and are never saved.

`Binaries/`, `Intermediate/`, `Saved/` and `DerivedDataCache/` are ignored by git.
