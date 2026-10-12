# BridgeDemo

A small Unreal Engine 5.6 project whose Blueprints were written entirely by a scripted agent
talking to the MCP server (`mcp-server/src/demo.ts`). It enables the plugin from `../../Plugin`
through `AdditionalPluginDirectories`, like the test project.

Build the editor target once:

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" BridgeDemoEditor Win64 Development -Project="<repo>\examples\BridgeDemo\BridgeDemo.uproject" -WaitMutex
```

Run the scenario (starts a headless editor, builds the Blueprints, saves them under
`Content/Blueprints`, restarts the editor and reloads them):

```bash
cd mcp-server
npm run build
node dist/demo.js --project ../examples/BridgeDemo/BridgeDemo.uproject
```

What the agent builds:

- `BP_RotatingPickup` (Actor): `RotationSpeed` variable; on Tick, `AddActorLocalRotation`
  with a rotator whose yaw is `DeltaSeconds * RotationSpeed`; on `ActorBeginOverlap`, cast the
  other actor to Pawn, print "Picked up!" and destroy itself.
- `BP_Door` (Actor): `IsOpen` and `OpenAngle` variables; a pure function `GetStateText` built
  with a Select node; a `Toggle` custom event that branches on `IsOpen`, rotates the actor to
  the open or closed angle and flips the flag; BeginPlay prints the state.

Open the project in the editor afterwards to inspect the graphs: they are ordinary compiled
Blueprints. `Binaries/`, `Intermediate/`, `Saved/` and `DerivedDataCache/` are ignored by git;
the generated `.uasset` files under `Content/Blueprints` are committed as a sample of the output.
