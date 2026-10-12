# Reverse engineering notes

No reverse engineering was needed so far. The Unreal Engine 5.6.1 launcher build ships the
engine source under `Engine/Source`, and every behaviour this plugin depends on was read directly
from it:

| Question | Where it was answered (UE 5.6.1) |
|---|---|
| How does a commandlet compile Blueprints? | `Editor/UnrealEd/Private/Commandlets/CompileAllBlueprintsCommandlet.cpp` |
| What does `TryCreateConnection` do with conversion nodes and replaced links? | `Runtime/Engine/Private/EdGraph/EdGraphSchema.cpp`, `Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp` |
| How are nodes spawned by the editor menu? | `Editor/BlueprintGraph/Private/BlueprintNodeSpawner.cpp` (`SpawnEdGraphNode`) |
| Is the action database populated synchronously? | `Editor/BlueprintGraph/Private/BlueprintActionDatabase.cpp` (`GetAllActions` calls `RefreshAll`) |
| Which text forms are valid pin defaults for vectors? | `Runtime/Core/Private/Misc/DefaultValueHelper.cpp` (`ParseVector`: `x,y,z` or `X=..,Y=..,Z=..`) |
| Where does the HTTP listener bind? | `Runtime/Online/HTTPServer/Private/HttpListener.cpp`, `HttpServerConfig.cpp` (`DefaultBindAddress`, default `localhost`) |
| How are custom events created with parameters? | `Editor/BlueprintGraph/Private/K2Node_CustomEvent.cpp` (`CreateFromFunction`) |
| How does paste assign new GUIDs and fix links? | `Editor/UnrealEd/Private/EdGraphUtilities.cpp` (`ImportNodesFromText`, `PostProcessPastedNodes`) |

If a future question cannot be answered from the source (binary-only modules, undocumented
serialization), document it here with: what was searched, how it was found, and the engine
version. Do not copy engine source or binary dumps into this repository.
