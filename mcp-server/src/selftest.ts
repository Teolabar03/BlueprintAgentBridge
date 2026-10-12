// Copyright (c) 2026 Matteo La Barbera. MIT License.
//
// End-to-end check without an MCP client: builds "BeginPlay -> Delay(2s) -> PrintString" in a
// fresh Blueprint, compiles it and prints the graph. Usage:
//   node dist/selftest.js --project path/to/Game.uproject [--mode auto|http|commandlet]
import { Bridge, loadConfig, log } from "./bridge.js";

async function main(): Promise<void> {
  const config = loadConfig(process.argv.slice(2), process.env);
  const bridge = new Bridge(config);
  log(bridge.describe());
  const started = Date.now();
  let failures = 0;

  const call = async (op: string, params: Record<string, unknown>, expectOk = true) => {
    const response = await bridge.call(op, params);
    const status = response.ok === expectOk ? "ok " : "FAIL";
    if (response.ok !== expectOk) failures++;
    const summary = response.ok ? "" : ` -> ${response.errors.join("; ")}`;
    console.log(`${status} ${op} (${response.elapsed_ms ?? "?"} ms)${summary}`);
    if (response.warnings?.length) console.log(`     warnings: ${response.warnings.join("; ")}`);
    return response;
  };

  const status = await call("bp_status", {});
  console.log(`     engine ${status.result.engine_version}, mode ${status.result.mode}`);

  const name = `BP_SelfTest_${Date.now().toString(36)}`;
  const bp = `/Game/BABTests/${name}`;
  await call("bp_create_blueprint", { path: "/Game/BABTests", name, parent_class: "Actor" });

  const beginPlay = await call("bp_add_node", { blueprint: bp, node: { type: "event", name: "BeginPlay" } });
  const delay = await call("bp_add_node", { blueprint: bp, node: { type: "call_function", function: "Delay" }, autowire_from: `${beginPlay.result.id}.then` });
  const print = await call("bp_add_node", { blueprint: bp, node: { type: "call_function", function: "PrintString" }, autowire_from: `${delay.result.id}.Completed` });
  await call("bp_set_pin_default", { blueprint: bp, node: delay.result.id, pin: "Duration", value: 2 });
  await call("bp_set_pin_default", { blueprint: bp, node: print.result.id, pin: "InString", value: "Hello from Blueprint Agent Bridge" });
  await call("bp_connect_pins", { blueprint: bp, from: `${delay.result.id}.Completed`, to: `${print.result.id}.InString` }, false);
  await call("bp_add_variable", { blueprint: bp, name: "Health", type: "float", default: 100, instance_editable: true });
  await call("bp_add_node", { blueprint: bp, node: { type: "variable_get", variable: "Health" } });
  await call("bp_add_function", { blueprint: bp, name: "Heal", inputs: [{ name: "Amount", type: "float" }], outputs: [{ name: "NewHealth", type: "float" }] });
  const search = await call("bp_search_nodes", { blueprint: bp, query: "print string", limit: 3 });
  console.log(`     first result: ${JSON.stringify((search.result.results as Array<Record<string, unknown>>)?.[0]?.add_node_spec)}`);
  await call("bp_auto_layout", { blueprint: bp });
  const compile = await call("bp_compile", { blueprint: bp });
  console.log(`     compile: ${compile.result.status}, ${compile.result.num_errors} errors, ${compile.result.num_warnings} warnings`);
  if (Number(compile.result.num_errors) !== 0) failures++;
  const graph = await call("bp_read_graph", { blueprint: bp });
  console.log("\n" + graph.result.text);

  await bridge.dispose();
  console.log(`\n${failures === 0 ? "SELFTEST PASSED" : `SELFTEST FAILED (${failures} failure(s))`} in ${((Date.now() - started) / 1000).toFixed(1)} s`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
