// Copyright (c) 2026 Matteo La Barbera. MIT License.
//
// Tool definitions. Each tool maps 1:1 to a plugin operation; the formatter turns the JSON
// result into compact text so the agent spends as few tokens as possible.
import { z } from "zod";
import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { describeEditorProcesses, type Bridge, type BridgeResponse } from "./bridge.js";

type Shape = Record<string, z.ZodTypeAny>;
type Json = Record<string, unknown>;

interface ToolDef {
  name: string;
  description: string;
  input: Shape;
  op?: string;
  readOnly?: boolean;
  destructive?: boolean;
  format?: (result: Json, args: Json) => string;
}

const blueprint = z.string().describe("Blueprint asset path such as /Game/Blueprints/BP_Player (the bare asset name works when it is unique).");
const graph = z.string().optional().describe("Graph name (EventGraph, a function or macro name). Defaults to the event graph.");
const node = z.string().describe("Node id from bp_read_graph (8 hex chars). A unique node title also works.");
const pinRef = z
  .union([z.string(), z.object({ node: z.string(), pin: z.string(), direction: z.enum(["in", "out"]).optional() })])
  .describe('Pin reference: "NODEID.PinName" or {node, pin}. Append ":in"/":out" to the pin name when a name exists in both directions.');
const vec2 = z.array(z.number()).length(2).describe("[x, y] graph position");
const paramList = z
  .array(z.object({ name: z.string(), type: z.string(), default: z.any().optional() }))
  .optional()
  .describe('Parameters like [{"name":"Amount","type":"float"}]. Types: bool, byte, int, int64, float, name, string, text, object:Actor, class:Actor, struct:Vector, enum:ECollisionChannel, array<T>, set<T>, map<K,V>, trailing & for by-ref.');

const nodeSpec = z
  .object({
    type: z
      .string()
      .describe(
        "call_function | event | custom_event | component_event | variable_get | variable_set | branch | sequence | multi_gate | macro | cast | class_cast | spawn_actor | knot | comment | self | make_array | make_struct | break_struct | select | switch_enum | switch_int | switch_string | switch_name | timeline | get_class_defaults | format_text | input_key | call_dispatcher | bind_dispatcher | unbind_dispatcher | clear_dispatcher | spawner | node_class"
      ),
    function: z.string().optional().describe('For call_function: "PrintString", "KismetSystemLibrary.PrintString" or "self.MyFunction".'),
    name: z.string().optional().describe("Event name (BeginPlay, Tick, ActorBeginOverlap, an interface function...), custom event name, timeline name, or input key."),
    class: z.string().optional().describe("Class for event owner, cast target, spawn_actor, get_class_defaults, variable or dispatcher owner (defaults to self)."),
    variable: z.string().optional().describe("Variable name for variable_get / variable_set."),
    component: z.string().optional().describe("Component name for component_event (e.g. Trigger)."),
    event: z.string().optional().describe("Delegate name for component_event (OnComponentBeginOverlap, OnComponentHit, OnClicked...)."),
    dispatcher: z.string().optional().describe("Event dispatcher name for call_dispatcher / bind_dispatcher / unbind_dispatcher / clear_dispatcher."),
    macro: z.string().optional().describe("Macro name for type=macro (ForEachLoop, ForLoop, WhileLoop, DoOnce, Gate, FlipFlop, IsValid, DoN...)."),
    library: z.string().optional().describe("Macro library asset path when the macro is not in StandardMacros or this Blueprint."),
    struct: z.string().optional().describe("Struct name for make_struct / break_struct (Vector, Rotator, Transform, LinearColor...)."),
    enum: z.string().optional().describe("Enum name for switch_enum."),
    text: z.string().optional().describe("Comment text for type=comment."),
    key: z.string().optional().describe("Key name for input_key (SpaceBar, E, LeftMouseButton...)."),
    inputs: paramList.describe("Parameters of a custom event."),
    pure: z.boolean().optional().describe("Pure cast (no exec pins)."),
    parent: z.boolean().optional().describe("call_function: call the parent implementation instead."),
    unique: z.boolean().optional().describe("custom_event: auto-rename when the name is taken."),
    signature: z.string().optional().describe("Spawner signature returned by bp_search_nodes (type=spawner)."),
    node_class: z.string().optional().describe("Raw node class for type=node_class, e.g. K2Node_MakeMap."),
    properties: z.record(z.any()).optional().describe("Reflected properties to set before pins are created (type=node_class)."),
    width: z.number().optional(),
    height: z.number().optional(),
  })
  .passthrough();

const variableFlags = {
  category: z.string().optional(),
  instance_editable: z.boolean().optional(),
  read_only: z.boolean().optional().describe("BlueprintReadOnly"),
  replicated: z.boolean().optional(),
  expose_on_spawn: z.boolean().optional(),
  private: z.boolean().optional(),
  tooltip: z.string().optional(),
};

function str(v: unknown): string {
  return typeof v === "string" ? v : v === undefined || v === null ? "" : JSON.stringify(v);
}

function jsonBlock(value: unknown): string {
  return JSON.stringify(value, null, 1).replace(/\n\s*/g, (m) => (m.length > 3 ? "\n" + " ".repeat(Math.min(m.length - 1, 4)) : m));
}

function formatCompile(result: Json): string {
  const lines = [`status: ${str(result.status)} | errors: ${str(result.num_errors)} | warnings: ${str(result.num_warnings)}`];
  const messages = (result.messages as Json[]) ?? [];
  for (const m of messages) {
    const where = m.node ? ` -> node ${str(m.node)}${m.node_title ? ` "${str(m.node_title)}"` : ""}${m.graph ? ` in ${str(m.graph)}` : ""}${m.pin ? ` pin ${str(m.pin)}` : ""}` : "";
    lines.push(`[${str(m.severity)}] ${str(m.message)}${where}`);
  }
  if (result.file) lines.push(`saved: ${str(result.file)}`);
  return lines.join("\n");
}

function formatSearch(result: Json): string {
  const results = (result.results as Json[]) ?? [];
  if (results.length === 0) return `No node matched "${str(result.query)}".`;
  const lines = [`${results.length} of ${str(result.total_matches)} matches for "${str(result.query)}":`];
  results.forEach((r, i) => {
    const flags = [r.pure ? "pure" : "", r.latent ? "latent" : "", r.static ? "static" : ""].filter(Boolean).join(", ");
    lines.push(`${i + 1}. ${str(r.title)} [${str(r.category)}]${flags ? ` (${flags})` : ""}`);
    if (r.function) lines.push(`   function: ${str(r.function)}`);
    lines.push(`   add_node_spec: ${JSON.stringify(r.add_node_spec)}`);
    if (r.pins) {
      const pins = (r.pins as Json[]).map((p) => `${p.dir === "in" ? "<" : ">"} ${str(p.name)}:${str(p.type)}${p.default !== undefined ? `=${str(p.default)}` : ""}`);
      lines.push(`   pins: ${pins.join("  ")}`);
    }
    if (r.tooltip) lines.push(`   ${str(r.tooltip).split("\n")[0].slice(0, 160)}`);
  });
  return lines.join("\n");
}

function formatAssets(result: Json): string {
  const assets = (result.assets as Json[]) ?? [];
  if (assets.length === 0) return `No Blueprint assets under ${str(result.path)}.`;
  const lines = assets.map((a) => `${str(a.path)}  (${str(a.parent_class) || "?"}${a.blueprint_type ? `, ${str(a.blueprint_type)}` : ""})`);
  lines.unshift(`${assets.length}${result.truncated ? ` of ${str(result.total)}` : ""} Blueprint(s) under ${str(result.path)}:`);
  return lines.join("\n");
}

function formatNodeResult(result: Json): string {
  const parts: string[] = [];
  if (result.text) parts.push(str(result.text).trimEnd());
  if (result.autowired !== undefined) parts.push(`autowired: ${result.autowired ? "yes" : "no"}`);
  if (result.graph) parts.push(`graph: ${str(result.graph)}`);
  return parts.join("\n");
}

function formatConnect(result: Json): string {
  const from = result.from as Json | undefined;
  const to = result.to as Json | undefined;
  const lines = [`${result.connected ? "Connected" : "Not connected"}: ${from ? `${str(from.node)}.${str(from.pin)}` : "?"} -> ${to ? `${str(to.node)}.${str(to.pin)}` : "?"} (${str(result.response)})`];
  const newNodes = (result.new_nodes as Json[]) ?? [];
  for (const n of newNodes) lines.push(`inserted conversion node ${str(n.id)} "${str(n.title)}"`);
  const replaced = (result.replaced_links as Json[]) ?? [];
  for (const r of replaced) lines.push(`replaced previous link to ${str(r.node)}.${str(r.pin)}`);
  return lines.join("\n");
}

function formatDefault(result: Json): string {
  if (typeof result.text === "string") {
    const rest = Object.fromEntries(Object.entries(result).filter(([k, v]) => k !== "text" && typeof v !== "object"));
    const meta = Object.entries(rest)
      .map(([k, v]) => `${k}: ${str(v)}`)
      .join(" | ");
    return result.text.trimEnd() + (meta ? `\n${meta}` : "");
  }
  return jsonBlock(result);
}

export const TOOLS: ToolDef[] = [
  {
    name: "bp_status",
    description: "Engine version, project, server mode and the list of available operations. Call it first to check the connection.",
    input: {},
    readOnly: true,
    format: (r) => `engine ${str(r.engine_version)} | project ${str(r.project)} | mode ${str(r.mode)} | plugin ${str(r.plugin_version)} | pid ${str(r.pid)}\noperations: ${(r.operations as string[])?.join(", ")}`,
  },
  {
    name: "bp_list_assets",
    description: "List Blueprint assets under a content path (default /Game), with parent class and type.",
    input: {
      path: z.string().optional().describe("Content path, e.g. /Game/Blueprints"),
      recursive: z.boolean().optional(),
      limit: z.number().optional(),
      name_filter: z.string().optional().describe("Substring filter on the asset name"),
      parent_class: z.string().optional().describe("Only Blueprints whose parent class is this (e.g. Actor, Pawn)"),
    },
    readOnly: true,
    format: formatAssets,
  },
  {
    name: "bp_create_blueprint",
    description: "Create a new Blueprint asset (Actor by default). Stays in memory until bp_save or save=true.",
    input: {
      name: z.string().describe("Asset name, e.g. BP_Door"),
      path: z.string().optional().describe("Content folder, default /Game"),
      parent_class: z.string().optional().describe("Parent class: Actor, Pawn, Character, ActorComponent, UserWidget, or a Blueprint path"),
      blueprint_type: z.enum(["normal", "const", "function_library", "interface", "macro_library"]).optional(),
      save: z.boolean().optional(),
    },
  },
  {
    name: "bp_describe_blueprint",
    description: "Summary of a Blueprint: parent, graphs, variables, components, functions, events, interfaces.",
    input: { blueprint, format: z.enum(["text", "json", "both"]).optional() },
    readOnly: true,
  },
  {
    name: "bp_read_graph",
    description:
      "Read a graph as compact text: one block per node with [id] title | kind, then its pins (< input, > output) with types, defaults and links (-> id.pin). Use the ids and pin names in the editing tools.",
    input: {
      blueprint,
      graph,
      format: z.enum(["text", "json", "both"]).optional(),
      include_hidden_pins: z.boolean().optional(),
      include_positions: z.boolean().optional(),
      include_guids: z.boolean().optional().describe("Add the full node GUID to the JSON output"),
      include_pin_types: z.boolean().optional().describe("Default true; false gives a shorter listing without pin types"),
    },
    readOnly: true,
  },
  {
    name: "bp_get_node",
    description: "Read one node (pins, defaults, links) by id.",
    input: { blueprint, node, graph },
    readOnly: true,
  },
  {
    name: "bp_search_nodes",
    description:
      "Search placeable nodes by words (functions, events, variables, macros, casts...). Returns add_node_spec objects you can pass directly to bp_add_node. Pass blueprint/graph for context-sensitive results.",
    input: {
      query: z.string().describe("Words to match, e.g. \"print string\", \"set actor location\", \"for each loop\", \"cast to character\""),
      blueprint: blueprint.optional(),
      graph,
      limit: z.number().optional().describe("Default 25"),
      kind: z.enum(["call_function", "event", "custom_event", "variable_get", "variable_set", "delegate", "node"]).optional(),
      include_pins: z.boolean().optional().describe("Include the pin list of each result"),
      context_sensitive: z.boolean().optional().describe("Filter out nodes not placeable in the given graph (default true)"),
      context_pin: pinRef.optional().describe("Only nodes that can connect to this pin"),
    },
    readOnly: true,
    format: formatSearch,
  },
  {
    name: "bp_add_node",
    description:
      'Add a node. Examples: {type:"event",name:"BeginPlay"}, {type:"call_function",function:"KismetSystemLibrary.PrintString"}, {type:"variable_get",variable:"Health"}, {type:"branch"}, {type:"macro",macro:"ForEachLoop"}, {type:"cast",class:"Character"}, {type:"custom_event",name:"OnHit",inputs:[{name:"Damage",type:"float"}]}. Returns the new node with its pin names.',
    input: {
      blueprint,
      graph,
      node: nodeSpec,
      pos: vec2.optional(),
      autowire_from: pinRef.optional().describe("Connect the new node to this pin automatically (like dragging a wire in the editor)."),
    },
    format: formatNodeResult,
  },
  {
    name: "bp_remove_node",
    description: "Remove a node. Reports the links that were broken.",
    input: { blueprint, node, graph },
    destructive: true,
  },
  {
    name: "bp_update_node",
    description: "Move a node, set its comment bubble, enable/disable it, rename a custom event or comment box.",
    input: {
      blueprint,
      node,
      graph,
      pos: vec2.optional(),
      comment: z.string().optional(),
      enabled: z.boolean().optional(),
      name: z.string().optional().describe("New name for custom events / comment boxes"),
      width: z.number().optional(),
      height: z.number().optional(),
    },
    format: formatNodeResult,
  },
  {
    name: "bp_connect_pins",
    description: "Connect an output pin to an input pin. Fails with the schema message when the types are incompatible; reports inserted conversion nodes and replaced links.",
    input: { blueprint, from: pinRef.describe("Output pin"), to: pinRef.describe("Input pin"), graph },
    format: formatConnect,
  },
  {
    name: "bp_disconnect_pins",
    description: "Break one link (from + to) or every link of a pin (pin).",
    input: { blueprint, from: pinRef.optional(), to: pinRef.optional(), pin: pinRef.optional(), graph },
  },
  {
    name: "bp_set_pin_default",
    description:
      "Set the default value of an input pin. value may be a number, boolean, string, enum entry/display name, class or asset path, [x,y,z] for vectors/rotators, or an object like {R:1,G:0,B:0,A:1} for structs. reset=true restores the default.",
    input: { blueprint, node, pin: z.string().describe("Pin name"), value: z.any().optional(), reset: z.boolean().optional(), graph },
    format: (r) => `${str(r.node)}.${str((r.pin as Json)?.name)} = ${str(r.default)}${r.reset ? " (reset)" : ""}`,
  },
  {
    name: "bp_add_pin",
    description: "Add an input pin to nodes that grow (Sequence, MakeArray, Select, Switch, MultiGate, math operators).",
    input: { blueprint, node, graph },
    format: formatNodeResult,
  },
  {
    name: "bp_remove_pin",
    description: "Remove a dynamic pin from a node that supports it. A connected pin needs force=true; the broken links are reported.",
    input: { blueprint, node, pin: z.string(), graph, force: z.boolean().optional() },
    destructive: true,
    format: formatNodeResult,
  },
  {
    name: "bp_split_pin",
    description: "Split a struct pin into its members (Vector -> X, Y, Z).",
    input: { blueprint, node, pin: z.string(), graph },
    format: formatNodeResult,
  },
  {
    name: "bp_recombine_pin",
    description: "Recombine a split struct pin.",
    input: { blueprint, node, pin: z.string(), graph },
    format: formatNodeResult,
  },
  {
    name: "bp_set_node_property",
    description: "Advanced: set a reflected property on a node (e.g. TargetType on a cast, bIsPureCast) and rebuild its pins.",
    input: { blueprint, node, property: z.string(), value: z.any(), reconstruct: z.boolean().optional(), graph },
    format: formatNodeResult,
  },
  {
    name: "bp_reconstruct_node",
    description: "Rebuild the pins of a node (after property changes or when pins look stale).",
    input: { blueprint, node, graph },
    format: formatNodeResult,
  },
  {
    name: "bp_export_text",
    description: "Export nodes in the editor copy/paste text format (all nodes of the graph or the given ids).",
    input: { blueprint, graph, nodes: z.array(z.string()).optional() },
    readOnly: true,
    format: (r) => str(r.text),
  },
  {
    name: "bp_import_text",
    description: "Paste nodes from the editor copy/paste text format (for example a snippet copied from the Blueprint editor or from a forum post).",
    input: { blueprint, graph, text: z.string(), pos: vec2.optional() },
    format: (r) => `imported ${str(r.count)} node(s):\n${((r.nodes as Json[]) ?? []).map((n) => `[${str(n.id)}] ${str(n.title)}`).join("\n")}`,
  },
  {
    name: "bp_auto_layout",
    description: "Re-arrange nodes left-to-right along their connections so the graph stays readable for humans.",
    input: {
      blueprint,
      graph,
      nodes: z.array(z.string()).optional().describe("Only these node ids"),
      origin: vec2.optional(),
      column_gap: z.number().optional().describe("Horizontal gap between columns, default 96"),
      row_gap: z.number().optional().describe("Vertical gap between nodes, default 48"),
    },
    format: (r) => `moved ${str(r.moved)} node(s)`,
  },
  {
    name: "bp_add_variable",
    description: "Add a member variable. type examples: float, int, bool, string, name, text, object:Actor, class:Actor, struct:Vector, enum:ECollisionChannel, array<float>, map<string,int>.",
    input: { blueprint, name: z.string(), type: z.string(), default: z.any().optional(), ...variableFlags },
  },
  {
    name: "bp_update_variable",
    description: "Rename, retype or change default/flags of a member variable.",
    input: { blueprint, name: z.string(), new_name: z.string().optional(), type: z.string().optional(), default: z.any().optional(), ...variableFlags },
  },
  {
    name: "bp_remove_variable",
    description: "Remove a member variable. Fails and lists the nodes that use it unless force=true.",
    input: { blueprint, name: z.string(), force: z.boolean().optional() },
    destructive: true,
  },
  {
    name: "bp_add_local_variable",
    description: "Add a local variable to a function graph.",
    input: { blueprint, graph: z.string().describe("Function graph name"), name: z.string(), type: z.string(), default: z.any().optional() },
  },
  {
    name: "bp_add_function",
    description: "Create a function graph with inputs and outputs. The result lists the entry/result node ids; then add nodes with graph=<function name>.",
    input: {
      blueprint,
      name: z.string(),
      inputs: paramList,
      outputs: paramList,
      pure: z.boolean().optional(),
      const: z.boolean().optional(),
      category: z.string().optional(),
      tooltip: z.string().optional(),
      access: z.enum(["public", "protected", "private"]).optional(),
      call_in_editor: z.boolean().optional(),
    },
    format: (r) => `function graph ${str(r.graph)} created (entry ${str(r.entry_node)}${r.result_node ? `, result ${str(r.result_node)}` : ""})\n${str(r.text).trimEnd()}`,
  },
  {
    name: "bp_remove_graph",
    description: "Remove a function, macro or event dispatcher graph. Fails and lists call sites unless force=true.",
    input: { blueprint, graph: z.string(), force: z.boolean().optional() },
    destructive: true,
  },
  {
    name: "bp_add_component",
    description: "Add a component to an Actor Blueprint: class (StaticMeshComponent, BoxComponent, PointLightComponent...), name, optional parent and initial properties ({RelativeLocation:[0,0,50], StaticMesh:\"/Engine/BasicShapes/Cube.Cube\"}). Scene components attach to the root unless parent is given.",
    input: {
      blueprint,
      class: z.string(),
      name: z.string().optional(),
      parent: z.string().optional().describe("Name of the component to attach to"),
      properties: z.record(z.any()).optional(),
      unique: z.boolean().optional().describe("Auto-rename when the name is taken"),
    },
    format: (r) => `component ${str(r.name)} (${str(r.class)})${r.parent ? ` under ${str(r.parent)}` : ""}\nproperties: ${jsonBlock(r.properties ?? {})}`,
  },
  {
    name: "bp_get_component",
    description: "Read a component: class, parent, children and the properties that differ from the class defaults (all=true lists every editable property, filter narrows by name).",
    input: { blueprint, component: z.string(), all: z.boolean().optional(), filter: z.string().optional() },
    readOnly: true,
  },
  {
    name: "bp_set_component_property",
    description: "Set properties on a component template: property + value, or a properties object (RelativeLocation, RelativeRotation, StaticMesh, BoxExtent, bGenerateOverlapEvents, Intensity...). Vectors as [x,y,z], assets as paths.",
    input: { blueprint, component: z.string(), property: z.string().optional(), value: z.any().optional(), properties: z.record(z.any()).optional() },
    format: (r) => `applied: ${((r.applied as string[]) ?? []).join(", ")}\n${jsonBlock((r.component as Json)?.properties ?? {})}`,
  },
  {
    name: "bp_remove_component",
    description: "Remove a component (children are re-parented). Fails and lists the graph nodes that use it unless force=true.",
    input: { blueprint, component: z.string(), force: z.boolean().optional() },
    destructive: true,
  },
  {
    name: "bp_get_class_defaults",
    description: "Class default values of the Blueprint: own variables plus inherited properties that differ from the parent (all=true for everything editable; filter by name).",
    input: { blueprint, all: z.boolean().optional(), filter: z.string().optional() },
    readOnly: true,
  },
  {
    name: "bp_set_class_default",
    description: "Set class default values (bReplicates, InitialLifeSpan, AutoReceiveInput, inherited variables...): property + value, or a properties object. Requires a compiled Blueprint.",
    input: { blueprint, property: z.string().optional(), value: z.any().optional(), properties: z.record(z.any()).optional() },
    format: (r) => `applied: ${((r.applied as string[]) ?? []).join(", ")}\n${jsonBlock(r.defaults ?? {})}`,
  },
  {
    name: "bp_add_interface",
    description: "Implement a Blueprint interface (asset path) or a native interface. Interface functions with outputs become function graphs; the others become events to add with bp_add_node type=event.",
    input: { blueprint, interface: z.string() },
  },
  {
    name: "bp_remove_interface",
    description: "Remove an implemented interface (preserve_functions=true keeps the graphs as plain functions).",
    input: { blueprint, interface: z.string(), preserve_functions: z.boolean().optional() },
    destructive: true,
  },
  {
    name: "bp_add_event_dispatcher",
    description: "Add an event dispatcher (multicast delegate) with optional params. Then use node types call_dispatcher, bind_dispatcher (connect a custom event OutputDelegate pin to its Event pin), unbind_dispatcher.",
    input: { blueprint, name: z.string(), params: paramList },
  },
  {
    name: "bp_get_timeline",
    description: "Read a timeline: length, flags and tracks with keys.",
    input: { blueprint, timeline: z.string() },
    readOnly: true,
  },
  {
    name: "bp_update_timeline",
    description: 'Edit a timeline created with bp_add_node type=timeline: length, auto_play, loop, and tracks [{name, type: float|vector|linear_color|event, keys: [[time, value], ...] (vectors: [time,[x,y,z]]), interp: linear|cubic|constant}]. Each track becomes an output pin of the timeline node.',
    input: {
      blueprint,
      timeline: z.string(),
      length: z.number().optional(),
      length_mode: z.enum(["timeline_length", "last_keyframe"]).optional(),
      auto_play: z.boolean().optional(),
      loop: z.boolean().optional(),
      replicated: z.boolean().optional(),
      ignore_time_dilation: z.boolean().optional(),
      tracks: z
        .array(
          z.object({
            name: z.string(),
            type: z.enum(["float", "vector", "linear_color", "event"]).optional(),
            keys: z.array(z.any()).optional(),
            interp: z.enum(["linear", "cubic", "constant"]).optional(),
          })
        )
        .optional(),
      remove_tracks: z.array(z.string()).optional(),
    },
    format: (r) => jsonBlock(r.timeline ?? r),
  },
  {
    name: "bp_undo",
    description: "Editor mode only: undo the last transaction if the bridge made it (each editing tool is one undo step in the open editor). force=true also undoes the user's own last action.",
    input: { force: z.boolean().optional() },
    format: (r) => `undone: ${str(r.undone)}${r.next ? `\nnext undo would be: ${str(r.next)}` : ""}`,
  },
  {
    name: "bp_redo",
    description: "Editor mode only: redo the last undone bridge transaction.",
    input: { force: z.boolean().optional() },
    format: (r) => `redone: ${str(r.redone)}`,
  },
  {
    name: "bp_compile",
    description: "Compile the Blueprint. Returns errors and warnings with the node id, title and graph they refer to. Always compile after editing.",
    input: { blueprint, save: z.boolean().optional().describe("Save after a successful compile") },
    format: formatCompile,
  },
  {
    name: "bp_save",
    description: "Compile and save the Blueprint asset to disk. Refuses to save with compile errors unless force=true.",
    input: { blueprint, compile: z.boolean().optional(), force: z.boolean().optional() },
    format: (r) => `${r.saved ? `saved ${str(r.file)}` : "not saved"}${r.compile ? `\n${formatCompile(r.compile as Json)}` : ""}`,
  },
];

export function registerTools(server: McpServer, bridge: Bridge): void {
  for (const def of TOOLS) {
    server.registerTool(
      def.name,
      {
        title: def.name,
        description: def.description,
        inputSchema: def.input,
        annotations: { readOnlyHint: !!def.readOnly, destructiveHint: !!def.destructive, idempotentHint: !!def.readOnly },
      },
      async (args: Json) => {
        let response: BridgeResponse;
        try {
          response = await bridge.call(def.op ?? def.name, args ?? {});
        } catch (error) {
          return { isError: true, content: [{ type: "text" as const, text: `Bridge error: ${(error as Error).message}` }] };
        }
        return toToolResult(def, response, args ?? {});
      }
    );
  }

  server.registerTool(
    "bp_batch",
    {
      title: "bp_batch",
      description: "Run several operations in one round trip (same op names and params as the other tools). Stops at the first failure unless stop_on_error=false.",
      inputSchema: {
        operations: z.array(z.object({ op: z.string(), params: z.record(z.any()) })),
        stop_on_error: z.boolean().optional(),
      },
      annotations: { readOnlyHint: false },
    },
    async (args) => {
      try {
        const response = await bridge.batch(args.operations as Array<{ op: string; params: Json }>, args.stop_on_error ?? true);
        const results = (response.results as BridgeResponse[]) ?? [];
        const lines = results.map((r, i) => {
          const def = TOOLS.find((t) => t.name === r.op);
          const body = r.ok ? (def?.format ? def.format(r.result, {}) : formatDefault(r.result)) : `ERROR: ${r.errors.join("; ")}`;
          const warnings = r.warnings?.length ? `\n  warnings: ${r.warnings.join("; ")}` : "";
          return `#${i + 1} ${r.op}: ${body}${warnings}`;
        });
        lines.unshift(`${response.ok ? "all ok" : "failed"} (${str(response.completed)}/${str(response.requested)} executed)`);
        return { isError: !response.ok, content: [{ type: "text" as const, text: lines.join("\n") }] };
      } catch (error) {
        return { isError: true, content: [{ type: "text" as const, text: `Bridge error: ${(error as Error).message}` }] };
      }
    }
  );

  server.registerTool(
    "bp_server",
    {
      title: "bp_server",
      description: "Manage the headless editor used by this MCP server: status, start, stop, restart, or show the last lines of its log. stop/restart only touch an editor this server started, unless force=true.",
      inputSchema: { action: z.enum(["status", "start", "stop", "restart", "log"]), lines: z.number().optional(), force: z.boolean().optional() },
      annotations: { readOnlyHint: false },
    },
    async (args) => {
      const describeStop = (outcome: "stopped" | "not-running" | "not-owned") =>
        outcome === "stopped" ? "stopped" : outcome === "not-running" ? "nothing was running" : "refused: that bridge was not started by this MCP server (an interactive editor?). Stop it from the editor, or pass force=true.";
      try {
        switch (args.action) {
          case "status": {
            const info = await bridge.health();
            const editors = describeEditorProcesses(bridge.detectEditors());
            const lines = [info ? `running (${bridge.describeOwnership()}): ${JSON.stringify(info)}` : "not running", bridge.describe(), `other editor processes on this project: ${editors}`];
            if (!info && bridge.detectEditors().some((e) => !e.isBridge)) {
              lines.push(`The project is open in an interactive editor without the bridge: ask the user to run "BAB.StartServer ${bridge.config.port}" in its console (or enable Auto Start Server in Editor Preferences) before editing.`);
            }
            return { content: [{ type: "text" as const, text: lines.join("\n") }] };
          }
          case "start":
            await bridge.ensureConnected();
            return { content: [{ type: "text" as const, text: `running (${bridge.describeOwnership()}): ${JSON.stringify(await bridge.health())}` }] };
          case "stop": {
            const outcome = await bridge.stopServer(args.force ?? false);
            return { isError: outcome === "not-owned", content: [{ type: "text" as const, text: describeStop(outcome) }] };
          }
          case "restart": {
            const outcome = await bridge.stopServer(args.force ?? false);
            if (outcome === "not-owned") {
              return { isError: true, content: [{ type: "text" as const, text: describeStop(outcome) }] };
            }
            await bridge.ensureConnected();
            return { content: [{ type: "text" as const, text: `restarted: ${JSON.stringify(await bridge.health())}` }] };
          }
          case "log":
            return { content: [{ type: "text" as const, text: bridge.tailLog(args.lines ?? 60) }] };
        }
      } catch (error) {
        return { isError: true, content: [{ type: "text" as const, text: `Bridge error: ${(error as Error).message}` }] };
      }
      return { content: [{ type: "text" as const, text: "unknown action" }] };
    }
  );
}

export function toToolResult(def: ToolDef, response: BridgeResponse, args: Json) {
  const warnings = response.warnings?.length ? `\nwarnings:\n${response.warnings.map((w) => `- ${w}`).join("\n")}` : "";
  if (!response.ok) {
    const extra = response.result && Object.keys(response.result).length > 0 ? `\n${jsonBlock(response.result)}` : "";
    return { isError: true, content: [{ type: "text" as const, text: `ERROR (${def.name}): ${response.errors.join("\n")}${extra}${warnings}` }] };
  }
  const text = def.format ? def.format(response.result, args) : formatDefault(response.result);
  return { content: [{ type: "text" as const, text: `${text}${warnings}` }] };
}
