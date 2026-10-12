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
- `BP_Door` (Actor): a cube `DoorMesh` component with a `Trigger` box under it; `IsOpen` and
  `OpenAngle` variables; a pure function `GetStateText` built with a Select node; an
  `OnDoorToggled` event dispatcher; a `DoorTimeline` whose `Alpha` track drives
  `SetActorRotation` through a Lerp; a `Toggle` custom event that plays or reverses the timeline,
  flips `IsOpen` and broadcasts the dispatcher; walking into the trigger calls `Toggle`;
  BeginPlay prints the state. `bReplicates` is set as a class default.

Open the project in the editor afterwards to inspect the graphs: they are ordinary compiled
Blueprints. `Binaries/`, `Intermediate/`, `Saved/` and `DerivedDataCache/` are ignored by git;
the generated `.uasset` files under `Content/Blueprints` are committed as a sample of the output.
