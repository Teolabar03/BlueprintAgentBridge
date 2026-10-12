#!/usr/bin/env node
// Copyright (c) 2026 Matteo La Barbera. MIT License.
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { Bridge, loadConfig, log } from "./bridge.js";
import { registerTools } from "./tools.js";

const INSTRUCTIONS = `Blueprint Agent Bridge: read and edit Unreal Engine Blueprints through the editor APIs.
Workflow: bp_status -> bp_describe_blueprint -> bp_read_graph -> (bp_search_nodes) -> bp_add_node / bp_connect_pins / bp_set_pin_default -> bp_compile -> fix errors -> bp_save.
Node ids are 8 hex characters shown as [ID] in bp_read_graph; pins are referenced as "ID.PinName".
Execution pins are named "execute" (input) and "then" (output); events only have "then".
bp_add_node returns the pin names of the new node, so you rarely need to re-read the graph.
Always compile before saving; bp_save refuses to write a Blueprint with compile errors.`;

async function main(): Promise<void> {
  const config = loadConfig(process.argv.slice(2), process.env);
  const bridge = new Bridge(config);
  log(bridge.describe());

  const server = new McpServer({ name: "blueprint-agent-bridge", version: "0.2.0" }, { instructions: INSTRUCTIONS });
  registerTools(server, bridge);

  let disposed = false;
  const shutdown = async () => {
    if (disposed) return;
    disposed = true;
    await bridge.dispose();
  };
  process.on("SIGINT", () => void shutdown().then(() => process.exit(0)));
  process.on("SIGTERM", () => void shutdown().then(() => process.exit(0)));
  process.stdin.on("close", () => void shutdown().then(() => process.exit(0)));

  const transport = new StdioServerTransport();
  await server.connect(transport);
  log("ready (stdio)");
}

main().catch((error) => {
  log(`fatal: ${(error as Error).stack ?? error}`);
  process.exit(1);
});
