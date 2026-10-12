---
name: blueprint-agent-bridge
description: Read and edit Unreal Engine Blueprints (event graphs, functions, variables, pins, compile errors) through the Blueprint Agent Bridge MCP tools (bp_*). Use whenever the user asks to inspect, create, wire, fix or refactor Blueprint logic without opening the Unreal editor UI.
---

# Blueprint Agent Bridge

You have `bp_*` tools that talk to a running Unreal Editor (headless or interactive) and edit
Blueprint graphs through the native editor API. Nothing is written to disk until `bp_save`.

## Workflow

1. `bp_status` once, to confirm the connection (the first call may take a minute: it starts a headless editor).
2. `bp_describe_blueprint` to learn parent class, variables, functions, components and existing events.
3. `bp_read_graph` on the graph you will touch. Keep the node ids: every edit refers to them.
4. Find nodes you are not sure about with `bp_search_nodes` (returns `add_node_spec` objects you can pass straight to `bp_add_node`).
5. Build: `bp_add_node` (with `autowire_from` when you are extending a chain), `bp_connect_pins`, `bp_set_pin_default`, `bp_add_variable`, `bp_add_function`.
6. `bp_compile`. Fix every error it reports (each message carries the node id and graph). Warnings about unconnected pins are usually real problems too.
7. `bp_auto_layout` if you added more than a couple of nodes, then `bp_save`.
8. Tell the user what changed, in terms of nodes and graphs, and whether the asset was saved.

Use `bp_batch` to send several operations in one call when you already know the exact sequence.

## Reading the text format

```
[A3F9C2D1] Event BeginPlay | event Actor.ReceiveBeginPlay @ 0,0
  > then : exec -> 7B2E4F10.execute
[7B2E4F10] Delay | call KismetSystemLibrary.Delay (latent) @ 420,0
  < execute : exec <- A3F9C2D1.then
  < Duration : float = 2.0
  > then (Completed) : exec
```

- `[ID]` is the node id, `|` separates the title from the node kind and target (function, variable, event, macro, class).
- `<` lines are input pins, `>` lines are output pins; `name : type`, then `= default` or `<-`/`->` links written as `ID.PinName`.
- Flags in parentheses: `(self)` the pin targets the Blueprint itself, `(adv)` advanced pin, `(hidden)`, `(orphaned)` pin left over after a signature change, `(split)`.
- `! error: ...` lines under a node are compiler messages from the last compile.

Pin references everywhere: `"ID.PinName"` or `{"node": "ID", "pin": "PinName"}`. Pin names are the internal names shown in the listing (`InString`, not `In String`), but display names and `exec`/`then` aliases are accepted. If a name exists on both sides, add `:in` or `:out`.

Execution pins: inputs are `execute`, outputs are `then` (events only have `then`). On latent calls such as Delay the `then` pin is labelled `Completed` and the listing shows `then (Completed)`; both names work. Branch has `True`/`False`; ForEachLoop has `LoopBody`/`Completed` and `Array`/`ArrayElement`/`ArrayIndex`.

## Node specs for bp_add_node

| Goal | Spec |
|---|---|
| Overridable event | `{"type":"event","name":"BeginPlay"}` (also Tick, ActorBeginOverlap, AnyDamage, EndPlay, Hit...) |
| Custom event | `{"type":"custom_event","name":"OnHit","inputs":[{"name":"Damage","type":"float"}]}` |
| Function call | `{"type":"call_function","function":"KismetSystemLibrary.PrintString"}`; own function: `"self.Heal"`; `"SetActorLocation"` resolves display names too |
| Variable get / set | `{"type":"variable_get","variable":"Health"}`, `{"type":"variable_set","variable":"Health"}` |
| Branch / Sequence | `{"type":"branch"}`, `{"type":"sequence"}` |
| Loops and flow macros | `{"type":"macro","macro":"ForEachLoop"}` (ForLoop, WhileLoop, DoOnce, Gate, FlipFlop, IsValid, DoN, ForLoopWithBreak) |
| Cast | `{"type":"cast","class":"Character"}` (`"pure":true` for a cast without exec pins) |
| Spawn actor | `{"type":"spawn_actor","class":"BP_Projectile"}` |
| Struct make / break | `{"type":"make_struct","struct":"Vector"}`, `{"type":"break_struct","struct":"Transform"}` |
| Switch | `{"type":"switch_enum","enum":"ECollisionChannel"}`, `switch_int`, `switch_string`, `switch_name` |
| Other | `self`, `knot`, `comment` (`text`), `make_array`, `select`, `multi_gate`, `timeline` (`name`), `get_class_defaults`, `format_text`, `input_key` (`key`) |
| Anything from search | `{"type":"spawner","signature":"..."}` or any `add_node_spec` returned by `bp_search_nodes` |

Function names: if a bare name is ambiguous the error lists the candidates as `Class.Function`; pick one. Math operators live in `KismetMathLibrary` (search "add float", "multiply vector"). Engine class names are the reflected names without prefix: `Actor`, `Pawn`, `StaticMeshComponent`; Blueprint classes by asset path or `BP_Name`.

## Defaults and types

- `bp_set_pin_default` accepts numbers, booleans, strings, enum entries (`ECC_Visibility` or `Visibility`), class and asset paths, `[x,y,z]` for Vector/Rotator pins and objects like `{"R":1,"G":0,"B":0,"A":1}` for other structs.
- Variable and parameter types: `bool`, `byte`, `int`, `int64`, `float`, `name`, `string`, `text`, `object:Actor`, `class:Actor`, `softobject:Texture2D`, `struct:Vector`, `enum:ECollisionChannel`, `array<float>`, `set<name>`, `map<string,int>`, by-ref with a trailing `&`. `float` is the Blueprint Float (double precision).
- Array, set and map inputs cannot have a default: connect a Make Array / Make Map node instead.

## Rules

- Never claim a Blueprint works before `bp_compile` reports zero errors.
- New Actor Blueprints contain template events (BeginPlay, ActorBeginOverlap, Tick) shown as `[disabled]`. `bp_add_node` with that event enables the template node instead of duplicating it; a disabled node is ignored by the compiler.
- Events are unique: adding an enabled event twice fails and the error names the existing node; connect to it instead.
- Events and latent calls (Delay, timers, async actions) only go in event graphs, not in functions.
- `bp_remove_node`, `bp_remove_variable` and `bp_remove_graph` report what they break; removing a used variable or graph needs `force:true`. Say so to the user before forcing.
- Do not save a Blueprint the user did not ask you to change. `bp_save` refuses to write with compile errors unless `force:true`; do not force it silently.
- If the bridge says the asset file is read-only (source control), stop and ask the user to check it out.
- When a connection is refused, read the schema message: it tells you the types. Insert a conversion (search "to string", "to text", "to float") or fix the pin you picked.
- Prefer `autowire_from` when extending a chain: it picks the right exec/data pin like dragging a wire in the editor, and the result says whether it connected.

## Example: "print a message two seconds after BeginPlay"

```
bp_add_node {blueprint:"/Game/BP_Door", node:{type:"event",name:"BeginPlay"}}                        -> id E1
bp_add_node {blueprint:"/Game/BP_Door", node:{type:"call_function",function:"Delay"}, autowire_from:"E1.then"}      -> id D1
bp_set_pin_default {blueprint:"/Game/BP_Door", node:"D1", pin:"Duration", value:2}
bp_add_node {blueprint:"/Game/BP_Door", node:{type:"call_function",function:"PrintString"}, autowire_from:"D1.Completed"} -> id P1
bp_set_pin_default {blueprint:"/Game/BP_Door", node:"P1", pin:"InString", value:"Door ready"}
bp_compile {blueprint:"/Game/BP_Door"}
bp_save {blueprint:"/Game/BP_Door"}
```
