/* mcp_proxy.c - Standalone MCP stdio-to-HTTP proxy for Pd-vibes
 *
 * Lightweight binary launched by Claude Desktop (or any MCP client using
 * stdio transport). Reads newline-delimited JSON-RPC from stdin, writes
 * responses to stdout.
 *
 * - initialize, tools/list, ping: handled locally (always succeeds)
 * - tools/call: forwarded via HTTP POST to Pd-vibes at localhost:4330/mcp
 * - If Pd-vibes isn't reachable, attempts to launch it automatically
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
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif
#endif

/* ---- constants ---- */
#define PROXY_BUF_SIZE       (1024 * 1024)
#define PROXY_LINE_SIZE      (1024 * 1024)
#define PROXY_CONNECT_TIMEOUT_SEC  2
#define PROXY_LAUNCH_WAIT_SEC      8
#define PROXY_LAUNCH_POLL_MS       200

static int proxy_port = MCP_DEFAULT_PORT;

/* ---- minimal HTTP client ---- */

/* POST json_body to localhost:proxy_port/mcp, return response body or NULL.
   Caller must free() the returned string. */
static char *proxy_http_post(const char *json_body)
{
    int fd = -1;
    char *buf = NULL;
    char *result = NULL;

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

    {
        int total = 0, n;
        while ((n = (int)recv(fd, buf + total,
                              PROXY_BUF_SIZE - total - 1, 0)) > 0)
            total += n;
        buf[total] = 0;
    }

    /* extract body after HTTP headers */
    {
        char *body = strstr(buf, "\r\n\r\n");
        if (body)
        {
            body += 4;
            result = strdup(body);
        }
    }

fail:
    if (buf) free(buf);
    if (fd >= 0) close(fd);
    return result;
}

/* ---- process management ---- */

/* find the 'pd' binary as a sibling of this proxy binary */
static const char *proxy_find_pd(void)
{
    static char pd_path[4096];

#ifdef __APPLE__
    {
        uint32_t size = sizeof(pd_path);
        if (_NSGetExecutablePath(pd_path, &size) == 0)
        {
            /* resolve symlinks */
            char resolved[4096];
            if (realpath(pd_path, resolved))
                strncpy(pd_path, resolved, sizeof(pd_path) - 1);

            char *last_slash = strrchr(pd_path, '/');
            if (last_slash)
            {
                strcpy(last_slash + 1, "pd");
                if (access(pd_path, X_OK) == 0)
                    return pd_path;
            }
        }
    }
#elif defined(__linux__)
    {
        ssize_t len = readlink("/proc/self/exe", pd_path,
            sizeof(pd_path) - 1);
        if (len > 0)
        {
            pd_path[len] = 0;
            char *last_slash = strrchr(pd_path, '/');
            if (last_slash)
            {
                strcpy(last_slash + 1, "pd");
                if (access(pd_path, X_OK) == 0)
                    return pd_path;
            }
        }
    }
#endif

    /* fallback: try PATH */
    if (
#ifndef _WIN32
        system("which pd > /dev/null 2>&1") == 0
#else
        system("where pd > NUL 2>&1") == 0
#endif
    )
        return "pd";

    return NULL;
}

/* try to launch Pd-vibes with MCP enabled, wait for the server to come up */
static int proxy_launch_pd(void)
{
    const char *pd = proxy_find_pd();
    if (!pd)
    {
        fprintf(stderr, "pd-vibes-mcp: cannot find pd binary\n");
        return 0;
    }

    fprintf(stderr, "pd-vibes-mcp: launching %s ...\n", pd);

#ifndef _WIN32
    pid_t pid = fork();
    if (pid < 0) return 0;

    if (pid == 0)
    {
        /* child: detach and exec pd with MCP enabled */
        setsid();

        /* close proxy's stdin/stdout so Pd doesn't inherit them */
        close(STDIN_FILENO);
        close(STDOUT_FILENO);

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", proxy_port);
        execlp(pd, "pd", "-mcpport", port_str, (char *)NULL);
        _exit(127);
    }

    /* parent: wait for Pd's MCP server to become reachable */
    {
        int waited_ms = 0;
        while (waited_ms < PROXY_LAUNCH_WAIT_SEC * 1000)
        {
            usleep(PROXY_LAUNCH_POLL_MS * 1000);
            waited_ms += PROXY_LAUNCH_POLL_MS;

            /* try a quick connection to the port */
            int tfd = socket(AF_INET, SOCK_STREAM, 0);
            if (tfd < 0) continue;

            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port = htons((unsigned short)proxy_port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (connect(tfd, (struct sockaddr *)&a, sizeof(a)) == 0)
            {
                close(tfd);
                fprintf(stderr,
                    "pd-vibes-mcp: Pd-vibes is up (took %d ms)\n",
                    waited_ms);
                return 1;
            }
            close(tfd);
        }
    }

    fprintf(stderr,
        "pd-vibes-mcp: timed out waiting for Pd-vibes to start\n");
#endif /* !_WIN32 */

    return 0;
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
    /* first attempt */
    char *response = proxy_http_post(original_request);

    /* if that failed, try launching Pd */
    if (!response)
    {
        fprintf(stderr,
            "pd-vibes-mcp: cannot reach Pd-vibes at localhost:%d, "
            "attempting to launch...\n", proxy_port);

        if (proxy_launch_pd())
            response = proxy_http_post(original_request);
    }

    if (!response)
    {
        return make_error(id, -32000,
            "Could not connect to Pd-vibes. "
            "Please launch Pd-vibes and enable the MCP checkbox "
            "(Media > MCP), then try again.");
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
                    "pd-vibes-mcp: invalid port %d\n", proxy_port);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--help") == 0 ||
                 strcmp(argv[i], "-h") == 0)
        {
            fprintf(stderr,
                "pd-vibes-mcp: MCP stdio proxy for Pd-vibes\n"
                "Usage: pd-vibes-mcp [--port N]\n"
                "  --port N   Pd-vibes MCP server port (default: %d)\n",
                MCP_DEFAULT_PORT);
            return 0;
        }
    }

    /* ignore SIGPIPE (broken pipe from stdout) */
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    fprintf(stderr, "pd-vibes-mcp: proxy started (port %d)\n",
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
    fprintf(stderr, "pd-vibes-mcp: stdin closed, exiting\n");
    return 0;
}
