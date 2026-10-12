// Copyright (c) 2026 Matteo La Barbera. MIT License.
//
// Scripted "agent": drives the real MCP server over stdio (the same protocol Claude Code or
// Codex use) and builds two gameplay Blueprints in a project, then saves them and verifies the
// saved assets by reloading them in a fresh headless editor.
//
//   node dist/demo.js --project ../examples/BridgeDemo/BridgeDemo.uproject
//
// Everything the script knows about the graphs comes from the text the tools return, exactly
// as a language model would read it.
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const projectIndex = args.indexOf("--project");
const project = projectIndex >= 0 ? resolve(args[projectIndex + 1]) : undefined;
if (!project) {
  console.error("usage: node dist/demo.js --project <path/to/Project.uproject>");
  process.exit(2);
}

let failures = 0;
const started = Date.now();

function section(title: string): void {
  console.log(`\n=== ${title} ===`);
}

async function main(): Promise<void> {
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [join(here, "index.js"), "--project", project!],
    stderr: "inherit",
  });
  const client = new Client({ name: "blueprint-agent-bridge-demo", version: "0.1.0" });
  await client.connect(transport);

  const tools = await client.listTools();
  console.log(`connected; ${tools.tools.length} tools available`);

  async function call(name: string, toolArgs: Record<string, unknown>, expectOk = true): Promise<string> {
    const t0 = Date.now();
    const result = await client.callTool({ name, arguments: toolArgs });
    const text = (result.content as Array<{ type: string; text?: string }>).map((c) => (c.type === "text" ? c.text ?? "" : "")).join("\n");
    const ok = !result.isError;
    const mark = ok === expectOk ? "ok  " : "FAIL";
    if (ok !== expectOk) failures++;
    console.log(`${mark} ${name} (${Date.now() - t0} ms)`);
    for (const line of text.split("\n").slice(0, 8)) console.log(`     ${line}`);
    if (text.split("\n").length > 8) console.log(`     ...`);
    return text;
  }

  const idOf = (text: string): string => {
    const match = text.match(/^\[([0-9A-F]{8})\]/m);
    if (!match) throw new Error(`no node id in:\n${text}`);
    return match[1];
  };

  section("status");
  await call("bp_status", {});

  // ---------------------------------------------------------------------------------------
  section("BP_RotatingPickup: spins every frame, destroys itself when a pawn touches it");
  const pickup = "/Game/Blueprints/BP_RotatingPickup";
  await call("bp_create_blueprint", { path: "/Game/Blueprints", name: "BP_RotatingPickup", parent_class: "Actor" });
  await call("bp_add_variable", { blueprint: pickup, name: "RotationSpeed", type: "float", default: 90, instance_editable: true, category: "Pickup", tooltip: "Degrees per second" });

  const tick = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "event", name: "Tick" } }));
  const addRot = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "call_function", function: "AddActorLocalRotation" }, autowire_from: `${tick}.then` }));
  const multiply = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "call_function", function: "KismetMathLibrary.Multiply_DoubleDouble" } }));
  const speed = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "variable_get", variable: "RotationSpeed" } }));
  const makeRot = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "call_function", function: "KismetMathLibrary.MakeRotator" } }));
  await call("bp_connect_pins", { blueprint: pickup, from: `${tick}.DeltaSeconds`, to: `${multiply}.A` });
  await call("bp_connect_pins", { blueprint: pickup, from: `${speed}.RotationSpeed`, to: `${multiply}.B` });
  await call("bp_connect_pins", { blueprint: pickup, from: `${multiply}.ReturnValue`, to: `${makeRot}.Yaw` });
  await call("bp_connect_pins", { blueprint: pickup, from: `${makeRot}.ReturnValue`, to: `${addRot}.DeltaRotation` });

  const overlap = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "event", name: "ActorBeginOverlap" } }));
  const castPawn = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "cast", class: "Pawn" }, autowire_from: `${overlap}.then` }));
  await call("bp_connect_pins", { blueprint: pickup, from: `${overlap}.OtherActor`, to: `${castPawn}.Object` });
  const print = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "call_function", function: "PrintString" }, autowire_from: `${castPawn}.then` }));
  await call("bp_set_pin_default", { blueprint: pickup, node: print, pin: "InString", value: "Picked up!" });
  await call("bp_set_pin_default", { blueprint: pickup, node: print, pin: "TextColor", value: { R: 0.2, G: 1, B: 0.2, A: 1 } });
  const destroy = idOf(await call("bp_add_node", { blueprint: pickup, node: { type: "call_function", function: "DestroyActor" }, autowire_from: `${print}.then` }));
  void destroy;

  // A wrong connection must be refused with a readable reason.
  await call("bp_connect_pins", { blueprint: pickup, from: `${tick}.DeltaSeconds`, to: `${addRot}.DeltaRotation` }, false);

  await call("bp_auto_layout", { blueprint: pickup });
  await call("bp_compile", { blueprint: pickup });

  // ---------------------------------------------------------------------------------------
  section("BP_Door: Toggle custom event, state function with a Select node");
  const door = "/Game/Blueprints/BP_Door";
  await call("bp_create_blueprint", { path: "/Game/Blueprints", name: "BP_Door", parent_class: "Actor" });
  await call("bp_add_variable", { blueprint: door, name: "IsOpen", type: "bool", default: false, category: "Door" });
  await call("bp_add_variable", { blueprint: door, name: "OpenAngle", type: "float", default: 90, instance_editable: true, category: "Door" });

  // Function GetStateText(): string, pure: Select(IsOpen, "Closed", "Open")
  const fn = await call("bp_add_function", { blueprint: door, name: "GetStateText", outputs: [{ name: "State", type: "string" }], pure: true, category: "Door" });
  const resultNode = fn.match(/result ([0-9A-F]{8})/)?.[1];
  if (!resultNode) throw new Error("no result node id in bp_add_function output");
  const select = idOf(await call("bp_add_node", { blueprint: door, graph: "GetStateText", node: { type: "select" } }));
  await call("bp_connect_pins", { blueprint: door, from: `${select}.Return Value`, to: `${resultNode}.State` });
  const isOpenGetter = idOf(await call("bp_add_node", { blueprint: door, graph: "GetStateText", node: { type: "variable_get", variable: "IsOpen" } }));
  await call("bp_connect_pins", { blueprint: door, from: `${isOpenGetter}.IsOpen`, to: `${select}.Index` });
  const selectText = await call("bp_get_node", { blueprint: door, node: select });
  const optionPins = [...selectText.matchAll(/^\s+< (Option \d|False|True) :/gm)].map((m) => m[1]);
  await call("bp_set_pin_default", { blueprint: door, node: select, pin: optionPins[0] ?? "Option 0", value: "Closed" });
  await call("bp_set_pin_default", { blueprint: door, node: select, pin: optionPins[1] ?? "Option 1", value: "Open" });
  await call("bp_auto_layout", { blueprint: door, graph: "GetStateText" });

  // Components: a cube slab and a trigger box under it
  await call("bp_add_component", { blueprint: door, class: "StaticMeshComponent", name: "DoorMesh", properties: { StaticMesh: "/Engine/BasicShapes/Cube.Cube", RelativeLocation: [0, 0, 100], RelativeScale3D: [0.1, 1.2, 2] } });
  await call("bp_add_component", { blueprint: door, class: "BoxComponent", name: "Trigger", parent: "DoorMesh", properties: { BoxExtent: [150, 150, 100], bGenerateOverlapEvents: true } });
  await call("bp_get_component", { blueprint: door, component: "Trigger" });
  await call("bp_set_class_default", { blueprint: door, property: "bReplicates", value: true });

  // Event dispatcher fired whenever the door toggles
  await call("bp_add_event_dispatcher", { blueprint: door, name: "OnDoorToggled", params: [{ name: "NowOpen", type: "bool" }] });

  // Timeline driving the rotation: Alpha 0 -> 1 in one second
  const timeline = idOf(await call("bp_add_node", { blueprint: door, node: { type: "timeline", name: "DoorTimeline" } }));
  await call("bp_update_timeline", { blueprint: door, timeline: "DoorTimeline", length: 1, tracks: [{ name: "Alpha", type: "float", keys: [[0, 0], [1, 1]], interp: "cubic" }] });
  const lerp = idOf(await call("bp_add_node", { blueprint: door, node: { type: "call_function", function: "KismetMathLibrary.Lerp" } }));
  const angle = idOf(await call("bp_add_node", { blueprint: door, node: { type: "variable_get", variable: "OpenAngle" } }));
  const makeDoorRot = idOf(await call("bp_add_node", { blueprint: door, node: { type: "call_function", function: "MakeRotator" } }));
  const setRot = idOf(await call("bp_add_node", { blueprint: door, node: { type: "call_function", function: "SetActorRotation" }, autowire_from: `${timeline}.Update` }));
  await call("bp_connect_pins", { blueprint: door, from: `${timeline}.Alpha`, to: `${lerp}.Alpha` });
  await call("bp_connect_pins", { blueprint: door, from: `${angle}.OpenAngle`, to: `${lerp}.B` });
  await call("bp_connect_pins", { blueprint: door, from: `${lerp}.ReturnValue`, to: `${makeDoorRot}.Yaw` });
  await call("bp_connect_pins", { blueprint: door, from: `${makeDoorRot}.ReturnValue`, to: `${setRot}.NewRotation` });

  // Toggle: Sequence -> [Branch(IsOpen) -> Reverse / Play the timeline] then [flip IsOpen -> broadcast OnDoorToggled]
  const toggle = idOf(await call("bp_add_node", { blueprint: door, node: { type: "custom_event", name: "Toggle" } }));
  const sequence = idOf(await call("bp_add_node", { blueprint: door, node: { type: "sequence" }, autowire_from: `${toggle}.then` }));
  const branch = idOf(await call("bp_add_node", { blueprint: door, node: { type: "branch" }, autowire_from: `${sequence}.then_0` }));
  const isOpen2 = idOf(await call("bp_add_node", { blueprint: door, node: { type: "variable_get", variable: "IsOpen" } }));
  await call("bp_connect_pins", { blueprint: door, from: `${isOpen2}.IsOpen`, to: `${branch}.Condition` });
  await call("bp_connect_pins", { blueprint: door, from: `${branch}.True`, to: `${timeline}.Reverse` });
  await call("bp_connect_pins", { blueprint: door, from: `${branch}.False`, to: `${timeline}.Play` });
  const setOpen = idOf(await call("bp_add_node", { blueprint: door, node: { type: "variable_set", variable: "IsOpen" }, autowire_from: `${sequence}.then_1` }));
  const not = idOf(await call("bp_add_node", { blueprint: door, node: { type: "call_function", function: "KismetMathLibrary.Not_PreBool" } }));
  await call("bp_connect_pins", { blueprint: door, from: `${isOpen2}.IsOpen`, to: `${not}.A` });
  await call("bp_connect_pins", { blueprint: door, from: `${not}.ReturnValue`, to: `${setOpen}.IsOpen` });
  const broadcast = idOf(await call("bp_add_node", { blueprint: door, node: { type: "call_dispatcher", dispatcher: "OnDoorToggled" }, autowire_from: `${setOpen}.then` }));
  await call("bp_connect_pins", { blueprint: door, from: `${setOpen}.IsOpen`, to: `${broadcast}.NowOpen` });

  // Walking into the trigger toggles the door
  const overlapDoor = idOf(await call("bp_add_node", { blueprint: door, node: { type: "component_event", component: "Trigger", event: "OnComponentBeginOverlap" } }));
  await call("bp_add_node", { blueprint: door, node: { type: "call_function", function: "self.Toggle" }, autowire_from: `${overlapDoor}.then` });

  // BeginPlay -> PrintString(GetStateText())
  const beginPlay = idOf(await call("bp_add_node", { blueprint: door, node: { type: "event", name: "BeginPlay" } }));
  const printState = idOf(await call("bp_add_node", { blueprint: door, node: { type: "call_function", function: "PrintString" }, autowire_from: `${beginPlay}.then` }));
  const getState = idOf(await call("bp_add_node", { blueprint: door, node: { type: "call_function", function: "self.GetStateText" } }));
  await call("bp_connect_pins", { blueprint: door, from: `${getState}.State`, to: `${printState}.InString` });

  await call("bp_auto_layout", { blueprint: door });
  await call("bp_compile", { blueprint: door });
  await call("bp_describe_blueprint", { blueprint: door });

  // ---------------------------------------------------------------------------------------
  section("search");
  await call("bp_search_nodes", { blueprint: door, query: "set actor location", limit: 3 });
  await call("bp_search_nodes", { blueprint: door, query: "for each loop", limit: 3, include_pins: true });

  // ---------------------------------------------------------------------------------------
  section("save both assets");
  await call("bp_save", { blueprint: pickup });
  await call("bp_save", { blueprint: door });

  // ---------------------------------------------------------------------------------------
  section("round trip: restart the headless editor and reload the saved assets");
  await call("bp_server", { action: "restart" });
  await call("bp_list_assets", { path: "/Game/Blueprints" });
  const reloaded = await call("bp_read_graph", { blueprint: door });
  for (const expected of ["Toggle", "Set Actor Rotation", "DoorTimeline", "> Alpha", "OnDoorToggled", "Trigger"]) {
    if (!reloaded.includes(expected)) {
      failures++;
      console.log(`FAIL reloaded BP_Door graph does not contain "${expected}"`);
    }
  }
  await call("bp_describe_blueprint", { blueprint: door });
  const recompile = await call("bp_compile", { blueprint: pickup });
  const recompile2 = await call("bp_compile", { blueprint: door });
  for (const text of [recompile, recompile2]) {
    if (!/errors: 0/.test(text)) {
      failures++;
      console.log("FAIL compile after reload reported errors");
    }
  }
  await call("bp_read_graph", { blueprint: door, graph: "GetStateText" });

  await client.close();
  console.log(`\n${failures === 0 ? "DEMO PASSED" : `DEMO FAILED (${failures} failure(s))`} in ${((Date.now() - started) / 1000).toFixed(1)} s`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
