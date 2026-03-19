/* mcp_register.h - Auto-register Pure Vibes as an MCP server with AI agents
 *
 * On startup, edits the AI agent's configuration file to add Pure Vibes
 * as a registered MCP server, pointing at the pd-mcp binary.
 */

#ifndef MCP_REGISTER_H
#define MCP_REGISTER_H

/* Register Pure Vibes as an MCP server with the given AI agent.
 * Currently supported agents: "Claude Desktop" (macOS, Windows).
 *
 * pd_mcp_path: absolute path to the pd-mcp binary.
 * agent_name:  name of the AI agent to register with.
 * is_release:  if nonzero, register as "Pure Vibes"; otherwise "Pure Vibes (Dev)".
 *
 * Returns 1 if registration was added, 0 if already present, -1 on error. */
int mcp_register_with_agent(const char *pd_mcp_path, const char *agent_name,
    int is_release);

#endif /* MCP_REGISTER_H */
