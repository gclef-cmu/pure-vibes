/* s_mcp.h - Native MCP (Model Context Protocol) server for Pure Data */

#ifndef S_MCP_H
#define S_MCP_H

#define MCP_DEFAULT_PORT 4330

void mcp_init(void);
void mcp_start(int port, int localhost_only);
void mcp_stop(void);
void mcp_free(void);

/* glob methods called from Tcl via pdsend */
void glob_mcp(void *dummy, t_symbol *s, int argc, t_atom *argv);
void glob_mcp_port(void *dummy, t_floatarg port);
void glob_mcp_network(void *dummy, t_floatarg allow);

#endif /* S_MCP_H */
