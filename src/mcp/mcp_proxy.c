/* mcp_proxy.c - Standalone MCP stdio-to-HTTP proxy for Pd-vibes
 *
 * Lightweight binary launched by Claude Desktop (or any MCP client using
 * stdio transport). Reads newline-delimited JSON-RPC from stdin, writes
 * responses to stdout.
 *
 * - initialize, tools/list, ping: handled locally (always succeeds)
 * - tools/call: forwarded via HTTP POST to Pd-vibes at localhost:4330/mcp
 * - If Pd-vibes isn't reachable, returns an error telling the user to
 *   launch Pd-vibes and enable MCP.
 *
 * Dependencies: cJSON + mcp_tools (shared). No Pd internals.
 */

#include "mcp_tools.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define close(fd) closesocket(fd)
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

/* ---- constants ---- */
#define PROXY_BUF_SIZE       (1024 * 1024)
#define PROXY_LINE_SIZE      (1024 * 1024)
#define PROXY_CONNECT_TIMEOUT_SEC  2
#define PROXY_IO_TIMEOUT_SEC       2
static int proxy_port = MCP_DEFAULT_PORT;

/* ---- minimal HTTP client ---- */

/* POST json_body to localhost:proxy_port/mcp, return response body or NULL.
   Caller must free() the returned string. */
static char *proxy_http_post(const char *json_body)
{
    int fd = -1;
    char *buf = NULL;
    char *result = NULL;
    int total = 0;
    int content_length = -1;
    char *body = NULL;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)proxy_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* set a connect timeout using non-blocking + select */
#ifndef _WIN32
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#else
    {
        unsigned long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
    }
#endif

    int conn = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (conn < 0)
    {
#ifndef _WIN32
        if (errno == EINPROGRESS)
#else
        if (WSAGetLastError() == WSAEWOULDBLOCK)
#endif
        {
            fd_set wfds;
            struct timeval tv;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = PROXY_CONNECT_TIMEOUT_SEC;
            tv.tv_usec = 0;
            if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0)
                goto fail;
            /* check for connect error */
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &elen);
            if (err != 0) goto fail;
        }
        else
            goto fail;
    }

    /* restore blocking mode for send/recv */
#ifndef _WIN32
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
#else
    {
        unsigned long mode = 0;
        ioctlsocket(fd, FIONBIO, &mode);
    }
#endif

    /* bound send/recv so an open but hung port fails quickly */
    {
        struct timeval tv;
        tv.tv_sec = PROXY_IO_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const void *)&tv, sizeof(tv));
    }

    /* send HTTP request */
    {
        int body_len = (int)strlen(json_body);
        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "POST /mcp HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n", body_len);

        send(fd, header, hlen, 0);
        send(fd, json_body, body_len, 0);
    }

    /* read response */
    buf = (char *)malloc(PROXY_BUF_SIZE);
    if (!buf) goto fail;

    while (total < PROXY_BUF_SIZE - 1)
    {
        int n = (int)recv(fd, buf + total, PROXY_BUF_SIZE - total - 1, 0);
        if (n <= 0)
            break;
        total += n;
        buf[total] = 0;

        if (!body)
        {
            char *header_end = strstr(buf, "\r\n\r\n");
            if (header_end)
            {
                char *content_length_hdr = strstr(buf, "Content-Length:");
                body = header_end + 4;
                if (content_length_hdr && content_length_hdr < header_end)
                    content_length = atoi(content_length_hdr + 15);
            }
        }

        if (body && content_length >= 0 && total >= (body - buf) + content_length)
            break;
    }

    if (body)
    {
        if (content_length >= 0)
        {
            result = (char *)malloc((size_t)content_length + 1);
            if (!result) goto fail;
            memcpy(result, body, (size_t)content_length);
            result[content_length] = 0;
        }
        else
            result = strdup(body);
    }

fail:
    if (buf) free(buf);
    if (fd >= 0) close(fd);
    return result;
}

/* ---- JSON-RPC helpers ---- */

static cJSON *make_error(cJSON *id, int code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddNullToObject(resp, "id");

    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    return resp;
}

static cJSON *handle_initialize(cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion",
        MCP_PROTOCOL_VERSION);

    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(info, "version", MCP_SERVER_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);

    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON *handle_ping(cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(resp, "result", cJSON_CreateObject());
    return resp;
}

static cJSON *handle_tools_list(cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", mcp_build_tools_list());
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

/* forward a tools/call request to Pd's HTTP MCP server */
static cJSON *handle_tools_call(cJSON *id, const char *original_request)
{
    char *response = proxy_http_post(original_request);

    if (!response)
    {
        fprintf(stderr,
            "pd-mcp: cannot reach Pd-vibes at localhost:%d; "
            "make sure Pd-vibes is already running and MCP is enabled\n",
            proxy_port);
        return make_error(id, -32000,
            "Could not connect to Pd-vibes. "
            "Make sure Pd-vibes is already running and the MCP checkbox "
            "(Media > MCP) is enabled, then try again.");
    }

    /* parse the response from Pd and return it */
    cJSON *parsed = cJSON_Parse(response);
    free(response);

    if (!parsed)
        return make_error(id, -32603, "Invalid response from Pd-vibes");

    return parsed;
}

/* ---- main request dispatcher ---- */

static void proxy_handle_request(const char *line)
{
    cJSON *req = cJSON_Parse(line);
    if (!req)
    {
        /* parse error — write error response */
        cJSON *err = make_error(NULL, -32700, "Parse error");
        char *out = cJSON_PrintUnformatted(err);
        fprintf(stdout, "%s\n", out);
        fflush(stdout);
        free(out);
        cJSON_Delete(err);
        return;
    }

    cJSON *id = cJSON_GetObjectItem(req, "id");
    cJSON *method = cJSON_GetObjectItem(req, "method");

    /* notifications (no id) — accept silently */
    if (!id)
    {
        cJSON_Delete(req);
        return;
    }

    if (!method || !cJSON_IsString(method))
    {
        cJSON *err = make_error(id, -32600, "Invalid request");
        char *out = cJSON_PrintUnformatted(err);
        fprintf(stdout, "%s\n", out);
        fflush(stdout);
        free(out);
        cJSON_Delete(err);
        cJSON_Delete(req);
        return;
    }

    const char *m = method->valuestring;
    cJSON *resp = NULL;

    if (strcmp(m, "initialize") == 0)
        resp = handle_initialize(id);
    else if (strcmp(m, "ping") == 0)
        resp = handle_ping(id);
    else if (strcmp(m, "tools/list") == 0)
        resp = handle_tools_list(id);
    else if (strcmp(m, "tools/call") == 0)
        resp = handle_tools_call(id, line);
    else
    {
        resp = make_error(id, -32601, "Method not found");
    }

    if (resp)
    {
        char *out = cJSON_PrintUnformatted(resp);
        fprintf(stdout, "%s\n", out);
        fflush(stdout);
        free(out);
        cJSON_Delete(resp);
    }

    cJSON_Delete(req);
}

/* ---- entry point ---- */

int main(int argc, char **argv)
{
    int i;

    /* parse optional arguments */
    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
        {
            proxy_port = atoi(argv[++i]);
            if (proxy_port < 1 || proxy_port > 65535)
            {
                fprintf(stderr,
                    "pd-mcp: invalid port %d\n", proxy_port);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--help") == 0 ||
                 strcmp(argv[i], "-h") == 0)
        {
            fprintf(stderr,
                "pd-mcp: MCP stdio proxy for Pd-vibes\n"
                "Usage: pd-mcp [--port N]\n"
                "  --port N   Pd-vibes MCP server port (default: %d)\n",
                MCP_DEFAULT_PORT);
            return 0;
        }
    }

    /* ignore SIGPIPE (broken pipe from stdout) */
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    fprintf(stderr, "pd-mcp: proxy started (port %d)\n",
        proxy_port);

    /* read lines from stdin, process each as JSON-RPC */
    char *line = (char *)malloc(PROXY_LINE_SIZE);
    if (!line) return 1;

    while (fgets(line, PROXY_LINE_SIZE, stdin))
    {
        /* strip trailing newline/CR */
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;

        if (len > 0)
            proxy_handle_request(line);
    }

    free(line);
    fprintf(stderr, "pd-mcp: stdin closed, exiting\n");
    return 0;
}
