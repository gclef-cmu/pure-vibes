/* mcp_server.h - Native MCP (Model Context Protocol) server for Pure Data
 *
 * Public API for the HTTP MCP server that runs inside Pd. Called from
 * s_main.c (startup/shutdown) and m_glob.c (Tcl ↔ C bridge).
 */

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include "m_pd.h" /* for t_symbol, t_atom, t_floatarg */
#include "mcp_tools.h" /* for MCP_DEFAULT_PORT */

void mcp_init(void);
int mcp_is_running(void);
void mcp_start(int port, int localhost_only);
void mcp_stop(void);
void mcp_free(void);

/* glob methods called from Tcl via pdsend */
void glob_mcp(void *dummy, t_symbol *s, int argc, t_atom *argv);
void glob_mcp_port(void *dummy, t_floatarg port);
void glob_mcp_network(void *dummy, t_floatarg allow);

#endif /* MCP_SERVER_H */
