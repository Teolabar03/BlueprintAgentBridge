# Graph format

The bridge exposes every graph in two representations produced by the same code
(`BABGraphSerializer.cpp`): a **JSON document** for tools and a **compact text listing** for
reading. Both use the same identifiers, so an agent can read the text and then address nodes
and pins by name in the editing operations.

## Design goals

- **Stable ids.** A node is identified by the first 8 hex digits of its `NodeGuid`. The GUID is
  stored in the asset, so ids survive save/load, renames and layout changes. Operations also accept
  the full GUID (with or without dashes) or a unique node title.
- **Pins by name.** Pins are addressed as `ID.PinName`. The internal `PinName` is shown (for
  example `InString`), because that is what the schema uses; display names (`In String`) and the
  aliases `exec`/`then` are accepted on input. When the same name exists on both sides,
  `PinName:in` / `PinName:out` disambiguates.
- **Compact.** Hidden pins (world context, latent info, split parents) are omitted unless
  `include_hidden_pins` is set. Unconnected output pins are listed so the agent knows what it can
  wire. Defaults are shown only on unconnected inputs.
- **Round trip.** Everything needed to rebuild the graph is present: node kind and target
  (function, event, variable, macro, class, struct), positions, pin types, defaults, links and
  the dynamic pins added by the user. For an exact copy of nodes the bridge also exposes the
  editor copy/paste format (`bp_export_text` / `bp_import_text`).

## Text listing

```
Blueprint /Game/Blueprints/BP_Door.BP_Door | parent: Actor | status: UpToDate
Graph EventGraph (ubergraph) | 3 nodes
# [id] title | kind detail @ x,y ; "<" input pin, ">" output pin, "=" default value, "<-"/"->" links to id.pin

[A3F9C2D1] Event BeginPlay | event Actor.ReceiveBeginPlay @ 0,0
  > then : exec -> 7B2E4F10.execute
[7B2E4F10] Delay | call KismetSystemLibrary.Delay (latent) @ 420,0
  < execute : exec <- A3F9C2D1.then
  < Duration : float = 2.0
  > Completed : exec -> C01D9E77.execute
[C01D9E77] Print String | call KismetSystemLibrary.PrintString @ 840,0
  < execute : exec <- 7B2E4F10.Completed
  < InString : string = "Door ready"
  < bPrintToScreen : bool = true (adv)
  < bPrintToLog : bool = true (adv)
  < TextColor : struct:LinearColor = (R=0.000000,G=0.660000,B=1.000000,A=1.000000) (adv)
  < Duration : float = 2.0 (adv)
  < Key : name = None (adv)
  > then : exec
```

Line grammar:

| Element | Meaning |
|---|---|
| `[ID] Title \| detail @ x,y` | node header; `detail` starts with the kind (`event`, `call`, `get`, `set`, `branch`, `macro`, `cast`, ...) followed by its target |
| `// text` after the header | node comment bubble |
| `[disabled]` | node is disabled (not compiled) |
| `! error: message` | compiler message attached to the node by the last compile |
| `< Name : type` | input pin; `= value` default (strings quoted), or `<- ID.Pin, ID.Pin` links |
| `> Name : type` | output pin; `-> ID.Pin` links |
| `Name (Label)` | the editor shows a label that differs from the pin name, e.g. `then (Completed)` on latent calls; both names are accepted |
| `(self)` | self/target pin that implicitly targets the Blueprint instance |
| `(adv)` | advanced-view pin (collapsed in the editor) |
| `(hidden)` | pin hidden by the node (only with `include_hidden_pins`) |
| `(orphaned)` | pin kept after a signature change; it should be reconnected or removed |
| `(split)` | struct pin that was split into sub pins (`Location_X`, ...) |

Node kinds and their detail text:

| kind | detail |
|---|---|
| `event` | `event Class.FunctionName` |
| `custom_event` | `custom_event Name` |
| `call_function` / `call_parent_function` | `call Class.Function` plus `(pure)`, `(latent)`, `(unresolved)` |
| `variable_get` / `variable_set` | `get Name` / `set Name`, `on Class` when not self |
| `branch`, `sequence`, `multi_gate`, `select`, `self`, `knot`, `make_array` | the kind |
| `macro` | `macro Name` |
| `cast` | `cast Class` plus `(pure)` |
| `spawn_actor` | `spawn_actor` (class is the `Class` pin default) |
| `make_struct` / `break_struct` | `make_struct Vector` |
| `switch` | `switch Enum` / `switch Integer` ... |
| `timeline` | `timeline Name` |
| `function_entry` / `function_result` | function terminators |
| `tunnel_entry` / `tunnel_exit` | macro terminators |
| `collapsed_graph` | `collapsed_graph Name` |
| `comment` | `comment WxH` (the text is the title) |
| `node` | any other class, shown by class name |

## Pin type strings

| string | K2 type |
|---|---|
| `exec` | execution |
| `bool`, `byte`, `int`, `int64`, `name`, `string`, `text` | primitives |
| `float` | `PC_Real` (both single and double precision are shown as `float`; new variables are double, the Blueprint Float) |
| `object:Actor`, `class:Actor`, `softobject:Texture2D`, `softclass:Actor`, `interface:Damageable` | object references; `object:self` for the self pin |
| `struct:Vector`, `struct:Transform` | structs, by reflected name (no `F` prefix) |
| `enum:ECollisionChannel` | enums (byte with enum) |
| `delegate`, `mcdelegate`, `wildcard`, `fieldpath` | special |
| `array<T>`, `set<T>`, `map<K,V>` | containers |
| trailing `&` | passed by reference |

The same grammar is used as input for variable and parameter types (`bp_add_variable`,
`bp_add_function`, custom event inputs). Bare names are also accepted when they resolve to a
struct, enum or class (`Vector`, `ECollisionChannel`, `Actor`).

## JSON document

`bp_read_graph` with `format: "json"` returns:

```json
{
  "blueprint": "/Game/Blueprints/BP_Door.BP_Door",
  "graph": { "name": "EventGraph", "kind": "ubergraph", "guid": "9F1C22A0" },
  "node_count": 3,
  "nodes": [
    {
      "id": "7B2E4F10", "class": "K2Node_CallFunction", "kind": "call_function",
      "title": "Delay", "function": "KismetSystemLibrary.Delay", "latent": true,
      "pos": [420, 0],
      "pins": [
        { "name": "execute", "dir": "in", "type": "exec", "links": [ { "node": "A3F9C2D1", "pin": "then" } ] },
        { "name": "Duration", "dir": "in", "type": "float", "default": "2.0", "links": [] },
        { "name": "Completed", "dir": "out", "type": "exec", "links": [ { "node": "C01D9E77", "pin": "execute" } ] }
      ]
    }
  ]
}
```

Optional node fields: `comment`, `enabled: false`, `compiler_message: {severity, message}`,
`guid` (with `include_guids`), plus the kind-specific details listed above. Optional pin fields:
`display`, `default`, `hidden`, `advanced`, `orphaned`, `connectable: false`, `parent`
(for sub pins), `split`, `is_self`.

## Why not the editor copy/paste text?

The T3D text produced by the editor (`FEdGraphUtilities::ExportNodesToText`) is complete but
verbose (every pin is serialized with GUIDs, flags and full object paths) and it is not meant to
be read. It remains available through `bp_export_text` / `bp_import_text` as a fallback and as a
way to paste snippets copied from a forum post or from another project.
