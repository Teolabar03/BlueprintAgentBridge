# Bridge protocol and operation reference

The plugin exposes one dispatcher (`FBABDispatcher`) with named operations. Every transport
(HTTP server, commandlet, automation tests, MCP server) sends the same JSON and gets the same JSON
back.

## Request / response

```json
{ "op": "bp_read_graph", "params": { "blueprint": "/Game/BP_Door", "graph": "EventGraph" }, "id": "optional" }
```

```json
{ "ok": true, "op": "bp_read_graph", "result": { ... }, "errors": [], "warnings": [], "elapsed_ms": 3.2, "id": "optional" }
```

Batch (stops at the first failure unless `stop_on_error` is false):

```json
{ "batch": [ { "op": "...", "params": { } }, ... ], "stop_on_error": true }
```

```json
{ "ok": false, "results": [ { ...first response... }, ... ], "completed": 2, "requested": 5 }
```

Failures never throw: `ok` is false and `errors[]` explains what to do. `warnings[]` carries
non-fatal information (read-only asset file, pin connected so the default is ignored, conversion
node inserted, ...).

## Transports

| Transport | How |
|---|---|
| HTTP (editor or headless) | `POST http://127.0.0.1:8765/rpc` with the request as body. `GET /health`, `GET /ops`, `POST /shutdown`. Loopback only. |
| Commandlet, one shot | `UnrealEditor-Cmd.exe Game.uproject -run=BlueprintAgentBridge -request=req.json -response=res.json` (exit code 0 when `ok`) |
| Commandlet, server | `UnrealEditor-Cmd.exe Game.uproject -run=BlueprintAgentBridge -serve -port=8765 [-idle=600]` prints `BAB_SERVER_READY` when listening |
| Interactive editor | start with `-BABServer [-BABPort=8765]`, enable *Auto Start Server* in Editor Preferences > Plugins > Blueprint Agent Bridge, or run the console command `BAB.StartServer [port]` |

## Common parameters

| name | meaning |
|---|---|
| `blueprint` | `/Game/Folder/BP_Name`, `/Game/Folder/BP_Name.BP_Name`, a `.uasset` path, or a bare asset name when unique |
| `graph` | graph name; omitted means the event graph |
| `node` | node id (8 hex), full GUID or unique title |
| pin reference | `"ID.PinName"` or `{ "node": "ID", "pin": "PinName", "direction": "in"\|"out" }`; `"PinName:out"` suffix form also works |
| `pos` | `[x, y]` or `{ "x": .., "y": .. }` |

## Operations

### Assets and compilation

| op | params | result |
|---|---|---|
| `bp_status` | | engine/project/plugin versions, mode, pid, server state, operation names |
| `bp_list_operations` | | descriptions of every operation |
| `bp_list_assets` | `path=/Game`, `recursive=true`, `limit=200`, `name_filter`, `parent_class` | `assets[] {path, object_path, name, class, parent_class, blueprint_type}`, `count`, `total`, `truncated` |
| `bp_create_blueprint` | `name`, `path=/Game`, `parent_class=Actor`, `blueprint_type=normal\|function_library\|interface\|macro_library\|const`, `save=false` | Blueprint summary (`bp_describe_blueprint` json) + `text`, `saved`, `file` when saved; `ok=false` when `save=true` and the write failed (the asset still exists in memory) |
| `bp_describe_blueprint` | `blueprint`, `format=text\|json\|both` | `path, name, parent_class, blueprint_type, status, dirty, graphs[], variables[], components[], functions[], macros[], event_dispatchers[], events[], interfaces[], timelines[]`, `text` |
| `bp_compile` | `blueprint`, `save=false` | `status`, `success`, `num_errors`, `num_warnings`, `messages[] {severity, message, node, node_title, graph, pin}` |
| `bp_save` | `blueprint`, `compile=true`, `force=false` | `saved`, `file`, `compile {...}` |

### Editor session (interactive editor only)

| op | params | result |
|---|---|---|
| `bp_undo` | `force` (also undo transactions not made by the bridge) | `undone` (transaction title), `next` |
| `bp_redo` | `force` | `redone` |

Inside a running editor every editing operation is wrapped in a transaction titled
`Blueprint Agent Bridge: <op>` (one Ctrl+Z step); failed operations are cancelled and leave no
partial change. Headless sessions keep no undo history, so these two operations fail there with a
clear message. `bp_list_operations` reports the kind of each operation (`read`, `edit`, `other`).

### Graph reading and search

| op | params | result |
|---|---|---|
| `bp_read_graph` | `blueprint`, `graph`, `format=text\|json\|both`, `include_hidden_pins`, `include_positions=true`, `include_guids`, `include_pin_types=true` | `text` and/or the JSON document (see graph-format.md) |
| `bp_get_node` | `blueprint`, `node`, `graph` | `node {...}`, `text`, `graph` |
| `bp_search_nodes` | `query`, `blueprint`, `graph`, `limit=25`, `kind`, `include_pins`, `context_sensitive=true`, `context_pin` | `results[] {title, category, tooltip, keywords, kind, node_class, function, pure, latent, static, add_node_spec, signature, pins[]}`, `count`, `total_matches` |

### Nodes and pins

| op | params | result |
|---|---|---|
| `bp_add_node` | `blueprint`, `graph`, `node {type, ...}` (or the spec fields inline), `pos`, `autowire_from` | `id`, `node {...}`, `text`, `graph`, `autowired` |
| `bp_remove_node` | `blueprint`, `node`, `graph` | `removed`, `title`, `graph`, `broken_links[] {pin, other_node, other_pin}` |
| `bp_update_node` | `blueprint`, `node`, `pos`, `comment`, `enabled`, `name` (custom events, comments), `width`, `height` | `node`, `text` |
| `bp_connect_pins` | `blueprint`, `from`, `to` (or `from_node`/`from_pin`/`to_node`/`to_pin`), `graph` | `connected`, `response` (`MAKE`, `MAKE_WITH_CONVERSION_NODE`, `MAKE_REPLACING_...`, `ALREADY_CONNECTED`), `message`, `from`, `to`, `new_nodes[]`, `replaced_links[]` |
| `bp_disconnect_pins` | `blueprint`, `from` + `to`, or `pin` (`node` + `pin`) | `broken[] {from, to}`, `count` |
| `bp_set_pin_default` | `blueprint`, `node`, `pin`, `value` (any JSON), `reset` | `pin {...}`, `node`, `default` |
| `bp_add_pin` | `blueprint`, `node` | `new_pins[]`, `node` |
| `bp_remove_pin` | `blueprint`, `node`, `pin`, `force` (needed when the pin is connected) | `broken_links[]`, `node` |
| `bp_split_pin` / `bp_recombine_pin` | `blueprint`, `node`, `pin` | `node` |
| `bp_set_node_property` | `blueprint`, `node`, `property`, `value`, `reconstruct=true` | `node`, `text` |
| `bp_reconstruct_node` | `blueprint`, `node` | `node`, `text` |
| `bp_export_text` | `blueprint`, `graph`, `nodes[]` | `text` (editor clipboard format), `node_count` |
| `bp_import_text` | `blueprint`, `graph`, `text`, `pos` | `nodes[]`, `count` |
| `bp_auto_layout` | `blueprint`, `graph`, `nodes[]`, `origin`, `column_gap`, `row_gap` | `moved`, `positions[] {id, title, pos}` |

### Node spec (`bp_add_node`)

| `type` | fields |
|---|---|
| `call_function` | `function` (`Func`, `Class.Func`, `Class:Func`, `self.Func`, `/Script/Module.Class:Func`), `class` (owner hint), `parent=true` for a parent call |
| `event` | `name` (`BeginPlay`, `ReceiveBeginPlay`, `Tick`, `ActorBeginOverlap`, ...), `class` (defaults to the parent class and implemented interfaces) |
| `custom_event` | `name`, `inputs[] {name, type}`, `unique` |
| `variable_get` / `variable_set` | `variable`, `class` (defaults to self) |
| `branch`, `sequence`, `multi_gate`, `self`, `knot`, `make_array`, `select`, `format_text`, `switch_int`, `switch_string`, `switch_name` | none |
| `macro` | `macro` (name in StandardMacros, this Blueprint, or `library`) |
| `cast` / `class_cast` | `class`, `pure` |
| `spawn_actor` | `class` (optional, sets the Class pin) |
| `make_struct` / `break_struct` | `struct` |
| `switch_enum` | `enum` |
| `timeline` | `name` |
| `get_class_defaults` | `class` |
| `comment` | `text`, `width`, `height` |
| `input_key` | `key` |
| `spawner` | `signature` from `bp_search_nodes` |
| `node_class` | `node_class` (`K2Node_...`), `properties {}` applied before pins are created |

### Variables, functions, graphs

| op | params | result |
|---|---|---|
| `bp_add_variable` | `blueprint`, `name`, `type`, `default`, `category`, `instance_editable`, `read_only`, `replicated`, `expose_on_spawn`, `private`, `tooltip` | `variable {...}`, `name`, `type` |
| `bp_update_variable` | `blueprint`, `name`, `new_name`, `type`, `default`, flags as above | `variable {...}`. Defaults are written to the class default object once the variable has been compiled (the engine clears the staging string on the first full compile) |
| `bp_remove_variable` | `blueprint`, `name`, `force` | `usages[] {blueprint, node, title, graph}` (this Blueprint and loaded dependents), `removed`, `removed_nodes` |
| `bp_add_local_variable` | `blueprint`, `graph` (function), `name`, `type`, `default` | `name`, `type`, `graph` |
| `bp_add_function` | `blueprint`, `name`, `inputs[]`, `outputs[]`, `pure`, `const`, `category`, `tooltip`, `access`, `call_in_editor` | `graph`, `entry_node`, `result_node`, `text` |
| `bp_remove_graph` | `blueprint`, `graph`, `force` | `usages[]`, `removed` |

## Safety rules implemented

- Edits happen in memory; only `bp_save` (or `save=true`) writes the `.uasset`.
- `bp_save` compiles first and refuses to write a Blueprint with errors unless `force=true`.
- Before writing, the file is checked for the read-only flag (source control) and for locks held
  by another process; both produce a clear error. `bp_add_node` warns early when the asset file
  is read-only.
- Destructive operations report their impact: broken links for nodes, node usages for variables,
  call sites (in this Blueprint and in loaded dependents) for graphs. Removing something that is
  still used needs `force=true`.
- The HTTP server binds to the loopback interface only.
