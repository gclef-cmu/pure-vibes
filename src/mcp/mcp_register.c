/* mcp_register.c - Auto-register Pure Vibes as an MCP server with AI agents
 *
 * Uses cJSON to read/modify/write the agent's JSON configuration file,
 * adding Pure Vibes to the list of MCP servers.
 */

#include "mcp_register.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <direct.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#include <pwd.h>
#define PATH_SEP '/'
#endif

/* forward declarations from Pd */
extern void post(const char *fmt, ...);
extern void logpost(const void *object, const int level, const char *fmt, ...);
#define PD_ERROR 1
#define PD_NORMAL 2
#define PD_DEBUG 3

/* ── helpers ────────────────────────────────────────────────────── */

/* Read entire file into malloc'd buffer. Returns NULL on failure. */
static char *read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, len, f) != len)
        { free(buf); fclose(f); return NULL; }
    buf[len] = '\0';
    fclose(f);
    if (out_len) *out_len = len;
    return buf;
}

/* Write buffer to file. Returns 0 on success, -1 on failure. */
static int write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, len, f) != len)
        { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Recursively create directories (like mkdir -p). */
static int mkdirs(const char *path)
{
    char tmp[2048];
    char *p;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == PATH_SEP)
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++)
    {
        if (*p == PATH_SEP)
        {
            *p = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = PATH_SEP;
        }
    }
#ifdef _WIN32
    return _mkdir(tmp);
#else
    return mkdir(tmp, 0755);
#endif
}

/* ── config path resolution ─────────────────────────────────────── */

/* Get the config file path for a given AI agent.
 * Returns 0 on success, -1 if agent/platform not supported. */
static int get_agent_config_path(const char *agent_name, char *buf, size_t bufsize)
{
    if (strcmp(agent_name, "Claude Desktop") != 0)
        return -1;

#ifdef __APPLE__
    {
        const char *home = getenv("HOME");
        if (!home)
        {
            struct passwd *pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
        }
        if (!home) return -1;
        snprintf(buf, bufsize,
            "%s/Library/Application Support/Claude/claude_desktop_config.json",
            home);
        return 0;
    }
#elif defined(_WIN32)
    {
        char appdata[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata)))
        {
            snprintf(buf, bufsize,
                "%s\\Claude\\claude_desktop_config.json", appdata);
            return 0;
        }
        return -1;
    }
#else
    /* Linux / other — not yet supported for Claude Desktop */
    (void)buf; (void)bufsize;
    return -1;
#endif
}

/* ── main registration function ─────────────────────────────────── */

int mcp_register_with_agent(const char *pd_mcp_path, const char *agent_name,
    int is_release)
{
    char config_path[2048];
    char config_dir[2048];
    char *file_data = NULL;
    cJSON *root = NULL, *servers = NULL, *entry = NULL;
    const char *server_name = is_release ? "Pure Vibes" : "Pure Vibes (Dev)";
    char *json_out = NULL;
    int result = -1;
    char *last_sep;

    /* resolve config file path */
    if (get_agent_config_path(agent_name, config_path, sizeof(config_path)) != 0)
        return -1;

    /* ensure the config directory exists */
    strncpy(config_dir, config_path, sizeof(config_dir));
    config_dir[sizeof(config_dir) - 1] = '\0';
    last_sep = strrchr(config_dir, PATH_SEP);
    if (last_sep) *last_sep = '\0';
    mkdirs(config_dir);

    /* read existing config or start fresh */
    file_data = read_file(config_path, NULL);
    if (file_data)
    {
        root = cJSON_Parse(file_data);
        free(file_data);
        if (!root)
        {
            logpost(NULL, PD_ERROR,
                "Pure Vibes: %s config exists but is not valid JSON, "
                "skipping auto-registration", agent_name);
            return -1;
        }
    }

    if (!root)
        root = cJSON_CreateObject();

    /* get or create mcpServers */
    servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
    if (!servers)
    {
        servers = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "mcpServers", servers);
    }

    /* check if already registered */
    entry = cJSON_GetObjectItemCaseSensitive(servers, server_name);
    if (entry)
    {
        /* already registered */
        logpost(NULL, PD_DEBUG,
            "Pure Vibes: already registered with %s (%s)",
            agent_name, config_path);
        cJSON_Delete(root);
        return 0;
    }

    /* add new entry: { "command": "<pd_mcp_path>" } */
    entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "command", pd_mcp_path);
    cJSON_AddItemToObject(servers, server_name, entry);

    /* write back */
    json_out = cJSON_Print(root);
    if (json_out)
    {
        if (write_file(config_path, json_out, strlen(json_out)) == 0)
        {
            logpost(NULL, PD_NORMAL,
                "Pure Vibes registered with %s using configuration %s",
                agent_name, config_path);
            result = 1;
        }
        else
        {
            logpost(NULL, PD_ERROR,
                "Pure Vibes: could not write %s", config_path);
        }
        free(json_out);
    }

    cJSON_Delete(root);
    return result;
}
