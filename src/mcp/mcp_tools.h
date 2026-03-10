/* mcp_tools.h - Shared MCP tool definitions and protocol constants
 *
 * Used by both the MCP server (linked into Pd) and the standalone
 * proxy binary (pd-mcp). Depends only on cJSON, no Pd internals.
 */

#ifndef MCP_TOOLS_H
#define MCP_TOOLS_H

#include "cJSON.h"

/* protocol constants */
#define MCP_PROTOCOL_VERSION "2025-03-26"
#define MCP_SERVER_NAME      "pd-vibes"
#define MCP_SERVER_VERSION   "0.56.2"
#define MCP_DEFAULT_PORT     4330

/* JSON schema helpers */
cJSON *mcp_prop_string(const char *desc);
cJSON *mcp_prop_int(const char *desc);
cJSON *mcp_prop_number(const char *desc);
cJSON *mcp_prop_bool(const char *desc);
cJSON *mcp_make_schema(const char *required[], int nrequired);
void mcp_schema_add_prop(cJSON *schema, const char *name, cJSON *prop);

/* build the complete MCP tools list as a cJSON array */
cJSON *mcp_build_tools_list(void);

#endif /* MCP_TOOLS_H */
