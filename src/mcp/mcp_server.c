/* s_mcp.c - Native MCP (Model Context Protocol) server for Pure Data
 *
 * Implements a minimal HTTP server that speaks the MCP Streamable HTTP
 * transport (JSON-RPC 2.0 over HTTP POST). Integrates into Pd's main
 * poll loop via sys_addpollfn() — no threads needed.
 */

#include "m_pd.h"
#include "m_imp.h"
#include "g_canvas.h"
#include "s_stuff.h"
#include "mcp_server.h"
#include "mcp_tools.h"
#include "mcp_audio.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
typedef int socklen_t;
#define close(fd) closesocket(fd)
#define strncasecmp _strnicmp
#define getcwd _getcwd
#define MCP_SOCKET_ERROR SOCKET_ERROR
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define MCP_SOCKET_ERROR (-1)
#endif

/* when set, canvas_vis() skips opening new windows — this prevents
   subcanvas_new() from flashing a Tk window during MCP object creation,
   which triggers a macOS AppKit NSMenuBar crash on rapid open/close */
int mcp_suppress_vis = 0;

/* ---- forward declarations for Pd internal functions ---- */
extern void canvas_obj(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_msg(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void glist_text(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_floatatom(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void canvas_symbolatom(t_glist *gl, t_symbol *s, int argc, t_atom *argv);
extern void glob_dsp(void *dummy, t_symbol *s, int argc, t_atom *argv);
extern void glob_open(t_pd *ignore, t_symbol *name, t_symbol *dir,
    t_floatarg f);
extern void glob_menunew(void *dummy, t_symbol *filesym, t_symbol *dirsym);

/* ---- constants ---- */
#define MCP_MAX_CLIENTS      8
#define MCP_MAX_REQUEST_SIZE (1024 * 1024)
#define MCP_SESSION_ID_LEN   32
#define MCP_BUF_INITIAL      4096
#define MCP_LOG_MAX_LINES    10000
#define MCP_LOG_LINE_MAX     512

/* ---- data structures ---- */
typedef struct _mcp_client {
    int fd;
    char *recv_buf;
    int recv_len;
    int recv_alloc;
    int active;
} t_mcp_client;

static struct {
    int listen_fd;
    int port;
    int localhost_only;
    int enabled;
    int running;
    t_mcp_client clients[MCP_MAX_CLIENTS];
    char session_id[MCP_SESSION_ID_LEN + 1];
} mcp_server = {
    .listen_fd = -1,
    .port = MCP_DEFAULT_PORT,
    .localhost_only = 1,
    .enabled = 0,
    .running = 0,
};

/* Pd log ring buffer for get_pd_log tool */
typedef struct {
    int level;
    char line[MCP_LOG_LINE_MAX];
} mcp_log_entry_t;
static mcp_log_entry_t mcp_log_ring[MCP_LOG_MAX_LINES];
static int mcp_log_head;
static int mcp_log_count;

static void mcp_logbuffer_append(int level, const char *msg)
{
    const char *p = msg;
    char buf[MCP_LOG_LINE_MAX];
    int i, len;
    while (*p)
    {
        const char *end = strchr(p, '\n');
        if (end)
        {
            len = (int)(end - p);
            if (len >= MCP_LOG_LINE_MAX) len = MCP_LOG_LINE_MAX - 1;
            memcpy(buf, p, len);
            buf[len] = 0;
            p = end + 1;
        }
        else
        {
            len = (int)strlen(p);
            if (len >= MCP_LOG_LINE_MAX) len = MCP_LOG_LINE_MAX - 1;
            memcpy(buf, p, len);
            buf[len] = 0;
            p += len;
        }
        if (mcp_log_count < MCP_LOG_MAX_LINES)
            mcp_log_count++;
        i = mcp_log_head % MCP_LOG_MAX_LINES;
        mcp_log_ring[i].level = level;
        strncpy(mcp_log_ring[i].line, buf, MCP_LOG_LINE_MAX - 1);
        mcp_log_ring[i].line[MCP_LOG_LINE_MAX - 1] = 0;
        mcp_log_head++;
    }
}

/* ---- utility: generate a random hex session ID ---- */
static void mcp_generate_session_id(char *buf, int len)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    /* use a mix of time and address for randomness (not crypto-secure,
       but fine for session IDs) */
    unsigned long seed = (unsigned long)clock_getlogicaltime();
    seed ^= (unsigned long)(intptr_t)buf;
    for (i = 0; i < len; i++)
    {
        seed = seed * 1103515245 + 12345;
        buf[i] = hex[(seed >> 16) & 0xf];
    }
    buf[len] = 0;
}

/* ==== TOOL IMPLEMENTATIONS ==== */

/* (tool definitions / JSON schemas are in mcp_tools.c) */

/* ---- stable pointer-based ID helpers ---- */

/* format a pointer as a stable hex string ID */
static const char *mcp_ptr_id(void *p, char *buf, size_t sz)
{
    snprintf(buf, sz, "%p", p);
    return buf;
}

/* recursively search for a canvas (patch or subpatch) by pointer ID */
static t_glist *mcp_find_canvas_recursive(t_glist *gl, const char *id)
{
    char buf[32];
    mcp_ptr_id(gl, buf, sizeof(buf));
    if (!strcmp(buf, id)) return gl;
    t_gobj *g;
    for (g = gl->gl_list; g; g = g->g_next)
    {
        if (pd_class(&g->g_pd) == canvas_class)
        {
            t_glist *found =
                mcp_find_canvas_recursive((t_glist *)g, id);
            if (found) return found;
        }
    }
    return NULL;
}

/* find any canvas (root or subpatch) by pointer ID string */
static t_glist *mcp_find_canvas_by_id(const char *id)
{
    t_glist *gl;
    if (!id) return NULL;
    for (gl = pd_this->pd_canvaslist; gl; gl = gl->gl_next)
    {
        t_glist *found = mcp_find_canvas_recursive(gl, id);
        if (found) return found;
    }
    return NULL;
}

/* find an object in a canvas by pointer ID string */
static t_gobj *mcp_find_gobj_by_id(t_glist *canvas, const char *id)
{
    t_gobj *g;
    char buf[32];
    if (!id) return NULL;
    for (g = canvas->gl_list; g; g = g->g_next)
    {
        mcp_ptr_id(g, buf, sizeof(buf));
        if (!strcmp(buf, id)) return g;
    }
    return NULL;
}

/* get the current sequential index of a gobj (needed for canvas_connect) */
static int mcp_gobj_index(t_glist *canvas, t_gobj *target)
{
    t_gobj *g;
    int i = 0;
    for (g = canvas->gl_list; g; g = g->g_next, i++)
        if (g == target) return i;
    return -1;
}

/* helper: count objects in canvas */
static int mcp_count_objects(t_glist *canvas)
{
    t_gobj *g;
    int count = 0;
    for (g = canvas->gl_list; g; g = g->g_next)
        count++;
    return count;
}

/* helper: create a JSON error result */
static cJSON *mcp_error_result(const char *msg)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "error", 1);
    cJSON_AddStringToObject(result, "message", msg);
    return result;
}

/* helper: create a JSON success result */
static cJSON *mcp_ok_result(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", 1);
    return result;
}

/* helper: wrap result as MCP tool content */
static cJSON *mcp_wrap_content(cJSON *data)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    char *text = cJSON_PrintUnformatted(data);
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    free(text);
    cJSON_Delete(data);
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    return result;
}

/* map te_type to string */
static const char *mcp_type_string(int te_type)
{
    switch (te_type)
    {
        case T_TEXT: return "text";
        case T_OBJECT: return "obj";
        case T_MESSAGE: return "msg";
        case T_ATOM: return "atom";
        default: return "unknown";
    }
}

/* map t_atomtype to string for method documentation */
static const char *mcp_atomtype_string(unsigned char atype)
{
    switch (atype)
    {
        case A_FLOAT: return "float";
        case A_SYMBOL: return "symbol";
        case A_POINTER: return "pointer";
        case A_DEFFLOAT: return "float?";
        case A_DEFSYMBOL: return "symbol?";
        case A_GIMME: return "...";
        case A_CANT: return "(internal)";
        default: return "unknown";
    }
}

/* ---- tool: list_patches ---- */
static cJSON *mcp_tool_list_patches(cJSON *args)
{
    (void)args;
    cJSON *result = cJSON_CreateArray();
    t_glist *gl;
    char idbuf[32];
    for (gl = pd_this->pd_canvaslist; gl; gl = gl->gl_next)
    {
            /* skip Pd's internal template patches */
        const char *name = gl->gl_name ? gl->gl_name->s_name : "";
        if (!strcmp(name, "_float_template") ||
            !strcmp(name, "_float_array_template") ||
            !strcmp(name, "_text_template"))
                continue;
        cJSON *patch = cJSON_CreateObject();
        cJSON_AddStringToObject(patch, "id",
            mcp_ptr_id(gl, idbuf, sizeof(idbuf)));
        cJSON_AddStringToObject(patch, "name",
            gl->gl_name ? gl->gl_name->s_name : "untitled");
        cJSON_AddStringToObject(patch, "dir",
            canvas_getdir(gl) ? canvas_getdir(gl)->s_name : "");
        cJSON_AddBoolToObject(patch, "dirty", gl->gl_dirty);
        cJSON_AddNumberToObject(patch, "width",
            gl->gl_screenx2 - gl->gl_screenx1);
        cJSON_AddNumberToObject(patch, "height",
            gl->gl_screeny2 - gl->gl_screeny1);
        cJSON_AddNumberToObject(patch, "num_objects",
            mcp_count_objects(gl));
        cJSON_AddItemToArray(result, patch);
    }
    return mcp_wrap_content(result);
}

/* ---- serialize a canvas to JSON (shared by get_patch_state & help) ---- */
static cJSON *mcp_serialize_canvas(t_glist *canvas)
{
    char idbuf[32];
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "patch_id",
        mcp_ptr_id(canvas, idbuf, sizeof(idbuf)));
    cJSON_AddStringToObject(result, "name",
        canvas->gl_name ? canvas->gl_name->s_name : "untitled");

    /* objects */
    cJSON *objects = cJSON_CreateArray();
    t_gobj *g;
    for (g = canvas->gl_list; g; g = g->g_next)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id",
            mcp_ptr_id(g, idbuf, sizeof(idbuf)));
        cJSON_AddStringToObject(obj, "class",
            class_getname(pd_class(&g->g_pd)));

        /* detect subpatches */
        if (pd_class(&g->g_pd) == canvas_class)
            cJSON_AddBoolToObject(obj, "is_subpatch", 1);

        t_object *ob = pd_checkobject(&g->g_pd);
        if (ob)
        {
            /* get text */
            char *buf = NULL;
            int len = 0;
            if (ob->te_binbuf)
            {
                binbuf_gettext(ob->te_binbuf, &buf, &len);
                if (buf)
                {
                    /* null-terminate */
                    char *text = (char *)getbytes(len + 1);
                    memcpy(text, buf, len);
                    text[len] = 0;
                    cJSON_AddStringToObject(obj, "text", text);
                    freebytes(text, len + 1);
                    freebytes(buf, len);
                }
            }
            cJSON_AddStringToObject(obj, "type",
                mcp_type_string(ob->te_type));
            cJSON_AddNumberToObject(obj, "x", ob->te_xpix);
            cJSON_AddNumberToObject(obj, "y", ob->te_ypix);
            cJSON_AddNumberToObject(obj, "inlets", obj_ninlets(ob));
            cJSON_AddNumberToObject(obj, "outlets", obj_noutlets(ob));
        }
        else
        {
            cJSON_AddStringToObject(obj, "type", "non-patchable");
        }
        cJSON_AddItemToArray(objects, obj);
    }
    cJSON_AddItemToObject(result, "objects", objects);

    /* connections */
    cJSON *connections = cJSON_CreateArray();
    t_linetraverser t;
    t_outconnect *oc;
    linetraverser_start(&t, canvas);
    while ((oc = linetraverser_next(&t)))
    {
        cJSON *conn = cJSON_CreateObject();
        cJSON_AddStringToObject(conn, "src_id",
            mcp_ptr_id(&t.tr_ob->ob_g, idbuf, sizeof(idbuf)));
        cJSON_AddNumberToObject(conn, "outlet", t.tr_outno);
        cJSON_AddStringToObject(conn, "dst_id",
            mcp_ptr_id(&t.tr_ob2->ob_g, idbuf, sizeof(idbuf)));
        cJSON_AddNumberToObject(conn, "inlet", t.tr_inno);
        cJSON_AddItemToArray(connections, conn);
    }
    cJSON_AddItemToObject(result, "connections", connections);

    return result;
}

/* ---- tool: get_patch_state ---- */
static cJSON *mcp_tool_get_patch_state(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    return mcp_wrap_content(mcp_serialize_canvas(canvas));
}

/* ---- tool: create_object ---- */
static cJSON *mcp_tool_create_object(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *type = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "type"));
    const char *text = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "text"));
    int x = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(args, "x"));
    int y = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(args, "y"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));
    if (!type)
        return mcp_wrap_content(mcp_error_result("type is required"));

    /* build argv: [x, y, text_atoms...] */
    t_binbuf *b = binbuf_new();
    t_atom coords[2];
    SETFLOAT(&coords[0], (t_float)x);
    SETFLOAT(&coords[1], (t_float)y);
    binbuf_add(b, 2, coords);

    if (text && *text)
    {
        t_binbuf *tb = binbuf_new();
        binbuf_text(tb, text, strlen(text));
        int tc = binbuf_getnatom(tb);
        t_atom *tv = binbuf_getvec(tb);
        binbuf_add(b, tc, tv);
        binbuf_free(tb);
    }

    int argc = binbuf_getnatom(b);
    t_atom *argv = binbuf_getvec(b);

    int dspwas = canvas_suspend_dsp();

    /* suppress canvas_vis() during object creation so that subcanvas_new()
       doesn't open a Tk window (which causes macOS AppKit crashes) */
    mcp_suppress_vis = 1;

    if (!strcmp(type, "obj"))
        canvas_obj(canvas, gensym("obj"), argc, argv);
    else if (!strcmp(type, "msg"))
        canvas_msg(canvas, gensym("msg"), argc, argv);
    else if (!strcmp(type, "text"))
        glist_text(canvas, gensym("text"), argc, argv);
    else if (!strcmp(type, "floatatom"))
        canvas_floatatom(canvas, gensym("floatatom"), argc, argv);
    else if (!strcmp(type, "symbolatom"))
        canvas_symbolatom(canvas, gensym("symbolatom"), argc, argv);
    else
    {
        mcp_suppress_vis = 0;
        binbuf_free(b);
        canvas_resume_dsp(dspwas);
        return mcp_wrap_content(mcp_error_result("unknown type"));
    }

    mcp_suppress_vis = 0;
    canvas_resume_dsp(dspwas);
    binbuf_free(b);

    canvas_dirty(canvas, 1);

    /* find the newly created object (last in the list) */
    t_gobj *last = NULL;
    t_gobj *g;
    for (g = canvas->gl_list; g; g = g->g_next)
        last = g;

    char idbuf[32];
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", 1);
    if (last)
        cJSON_AddStringToObject(result, "object_id",
            mcp_ptr_id(last, idbuf, sizeof(idbuf)));
    cJSON_AddNumberToObject(result, "num_objects",
        mcp_count_objects(canvas));
    return mcp_wrap_content(result);
}

/* ---- tool: delete_object ---- */
static cJSON *mcp_tool_delete_object(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *object_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "object_id"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    t_gobj *g = mcp_find_gobj_by_id(canvas, object_id);
    if (!g)
        return mcp_wrap_content(mcp_error_result("object not found"));

    int dspwas = canvas_suspend_dsp();
    glist_delete(canvas, g);
    canvas_resume_dsp(dspwas);
    canvas_dirty(canvas, 1);

    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: modify_object ---- */
static cJSON *mcp_tool_modify_object(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *object_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "object_id"));
    const char *text = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "text"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));
    if (!text)
        return mcp_wrap_content(mcp_error_result("text is required"));

    t_gobj *g = mcp_find_gobj_by_id(canvas, object_id);
    if (!g)
        return mcp_wrap_content(mcp_error_result("object not found"));

    t_object *ob = pd_checkobject(&g->g_pd);
    if (!ob)
        return mcp_wrap_content(
            mcp_error_result("object is not patchable"));

    int dspwas = canvas_suspend_dsp();
    text_setto(ob, canvas, text, strlen(text));
    canvas_resume_dsp(dspwas);
    canvas_dirty(canvas, 1);

    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: move_object ---- */
static cJSON *mcp_tool_move_object(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *object_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "object_id"));
    int x = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(args, "x"));
    int y = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(args, "y"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    t_gobj *g = mcp_find_gobj_by_id(canvas, object_id);
    if (!g)
        return mcp_wrap_content(mcp_error_result("object not found"));

    t_object *ob = pd_checkobject(&g->g_pd);
    if (!ob)
        return mcp_wrap_content(
            mcp_error_result("object is not patchable"));

    int dx = x - ob->te_xpix;
    int dy = y - ob->te_ypix;
    if (dx || dy)
        gobj_displace(g, canvas, dx, dy);
    canvas_dirty(canvas, 1);

    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: connect ---- */
static cJSON *mcp_tool_connect(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *src_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "src_id"));
    int outlet = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(args, "outlet"));
    const char *dst_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "dst_id"));
    int inlet = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(args, "inlet"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    t_gobj *src = mcp_find_gobj_by_id(canvas, src_id);
    t_gobj *dst = mcp_find_gobj_by_id(canvas, dst_id);
    if (!src || !dst)
        return mcp_wrap_content(mcp_error_result("object not found"));

    int src_idx = mcp_gobj_index(canvas, src);
    int dst_idx = mcp_gobj_index(canvas, dst);

    int dspwas = canvas_suspend_dsp();
    canvas_connect(canvas, src_idx, outlet, dst_idx, inlet);
    canvas_resume_dsp(dspwas);
    canvas_dirty(canvas, 1);

    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: disconnect ---- */
static cJSON *mcp_tool_disconnect(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *src_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "src_id"));
    int outlet = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(args, "outlet"));
    const char *dst_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "dst_id"));
    int inlet = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(args, "inlet"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    t_gobj *src = mcp_find_gobj_by_id(canvas, src_id);
    t_gobj *dst = mcp_find_gobj_by_id(canvas, dst_id);
    if (!src || !dst)
        return mcp_wrap_content(mcp_error_result("object not found"));

    int src_idx = mcp_gobj_index(canvas, src);
    int dst_idx = mcp_gobj_index(canvas, dst);

    canvas_disconnect(canvas, src_idx, outlet, dst_idx, inlet);
    canvas_dirty(canvas, 1);

    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: clear_patch ---- */
static cJSON *mcp_tool_clear_patch(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    int dspwas = canvas_suspend_dsp();
    glist_clear(canvas);
    canvas_resume_dsp(dspwas);
    canvas_dirty(canvas, 1);

    return mcp_wrap_content(mcp_ok_result());
}

/* forward declaration for batch_update */
static cJSON *mcp_dispatch_tool(const char *name, cJSON *args);

/* ---- tool: batch_update ---- */
static cJSON *mcp_tool_batch_update(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    cJSON *operations = cJSON_GetObjectItem(args, "operations");

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));
    if (!cJSON_IsArray(operations))
        return mcp_wrap_content(
            mcp_error_result("operations must be an array"));

    int dspwas = canvas_suspend_dsp();

    cJSON *results = cJSON_CreateArray();
    cJSON *op;
    cJSON_ArrayForEach(op, operations)
    {
        const char *tool_name = cJSON_GetStringValue(
            cJSON_GetObjectItem(op, "tool"));
        cJSON *tool_args = cJSON_GetObjectItem(op, "args");
        if (!tool_name)
        {
            cJSON_AddItemToArray(results,
                mcp_error_result("operation missing 'tool' field"));
            continue;
        }
        /* inject patch_id into args if not present */
        if (tool_args && !cJSON_GetObjectItem(tool_args, "patch_id"))
            cJSON_AddStringToObject(tool_args, "patch_id", patch_id);

        cJSON *r = mcp_dispatch_tool(tool_name, tool_args);
        cJSON_AddItemToArray(results, r);
    }

    canvas_resume_dsp(dspwas);
    canvas_dirty(canvas, 1);

    /* wrap the batch results */
    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddBoolToObject(wrapper, "success", 1);
    cJSON_AddItemToObject(wrapper, "results", results);
    return mcp_wrap_content(wrapper);
}

/* ---- tool: send_message ---- */
static cJSON *mcp_tool_send_message(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *object_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "object_id"));
    const char *message = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "message"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));
    if (!message)
        return mcp_wrap_content(mcp_error_result("message is required"));

    t_gobj *g = mcp_find_gobj_by_id(canvas, object_id);
    if (!g)
        return mcp_wrap_content(mcp_error_result("object not found"));

    /* parse message into atoms */
    t_binbuf *b = binbuf_new();
    binbuf_text(b, message, strlen(message));
    int argc = binbuf_getnatom(b);
    t_atom *argv = binbuf_getvec(b);

    if (argc > 0)
    {
        if (argv[0].a_type == A_SYMBOL)
        {
            t_symbol *sel = atom_getsymbol(&argv[0]);
            if (sel == gensym("bang") && argc == 1)
                pd_bang(&g->g_pd);
            else
                pd_typedmess(&g->g_pd, sel, argc - 1, argv + 1);
        }
        else if (argv[0].a_type == A_FLOAT)
        {
            if (argc == 1)
                pd_float(&g->g_pd, atom_getfloat(&argv[0]));
            else
                pd_list(&g->g_pd, &s_list, argc, argv);
        }
    }

    binbuf_free(b);
    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: send_bang ---- */
static cJSON *mcp_tool_send_bang(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *object_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "object_id"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    t_gobj *g = mcp_find_gobj_by_id(canvas, object_id);
    if (!g)
        return mcp_wrap_content(mcp_error_result("object not found"));

    pd_bang(&g->g_pd);
    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: set_number ---- */
static cJSON *mcp_tool_set_number(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *object_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "object_id"));
    double value = cJSON_GetNumberValue(
        cJSON_GetObjectItem(args, "value"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    t_gobj *g = mcp_find_gobj_by_id(canvas, object_id);
    if (!g)
        return mcp_wrap_content(mcp_error_result("object not found"));

    pd_float(&g->g_pd, (t_float)value);
    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: set_dsp ---- */
static cJSON *mcp_tool_set_dsp(cJSON *args)
{
    cJSON *enabled_item = cJSON_GetObjectItem(args, "enabled");
    int enabled = cJSON_IsTrue(enabled_item);

    t_atom a;
    SETFLOAT(&a, (t_float)enabled);
    glob_dsp(NULL, gensym("dsp"), 1, &a);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", 1);
    cJSON_AddBoolToObject(result, "dsp_running", enabled);
    return mcp_wrap_content(result);
}

/* ---- tool: get_dsp_state ---- */
static cJSON *mcp_tool_get_dsp_state(cJSON *args)
{
    (void)args;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "dsp_running", THISGUI->i_dspstate != 0);
    return mcp_wrap_content(result);
}

/* ---- tool: get_selection ---- */
static cJSON *mcp_tool_get_selection(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    char idbuf[32];
    cJSON *result = cJSON_CreateArray();
    if (canvas->gl_editor)
    {
        t_selection *sel;
        for (sel = canvas->gl_editor->e_selection; sel; sel = sel->sel_next)
        {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id",
                mcp_ptr_id(sel->sel_what, idbuf, sizeof(idbuf)));
            cJSON_AddStringToObject(item, "class",
                class_getname(pd_class(&sel->sel_what->g_pd)));

            t_object *ob = pd_checkobject(&sel->sel_what->g_pd);
            if (ob)
            {
                cJSON_AddNumberToObject(item, "x", ob->te_xpix);
                cJSON_AddNumberToObject(item, "y", ob->te_ypix);
            }
            cJSON_AddItemToArray(result, item);
        }
    }
    return mcp_wrap_content(result);
}

/* ---- tool: list_object_names ---- */
/* names to skip (infrastructure classes, not user-facing) */
static int mcp_skip_class(const char *name)
{
    /* skip internal infrastructure classes */
    return (!strcmp(name, "objectmaker") ||
            !strcmp(name, "canvasmaker") ||
            !strcmp(name, "vinlet") ||
            !strcmp(name, "voutlet") ||
            !strcmp(name, "vinlet~") ||
            !strcmp(name, "voutlet~") ||
            !strcmp(name, "savestate") ||
            !strcmp(name, "garray"));
}

static cJSON *mcp_tool_list_object_names(cJSON *args)
{
    (void)args;
    cJSON *internals = cJSON_CreateArray();
    cJSON *externals = cJSON_CreateArray();
    t_class *c;

    for (c = class_getfirst(); c; c = c->c_next)
    {
        if (!c->c_patchable) continue;
        const char *name = c->c_name->s_name;
        if (mcp_skip_class(name)) continue;

        /* c_externdir is &s_ (empty) for built-in classes */
        if (c->c_externdir == &s_)
            cJSON_AddItemToArray(internals, cJSON_CreateString(name));
        else
            cJSON_AddItemToArray(externals, cJSON_CreateString(name));
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "internals", internals);
    cJSON_AddItemToObject(result, "externals", externals);
    return mcp_wrap_content(result);
}

/* ---- open a help patch silently, serialize, and close ---- */
static cJSON *mcp_load_help_patch(const char *helpname, const char *helpdir)
{
    char realname[MAXPDSTRING], dirbuf[MAXPDSTRING], *basename;
    const char *usedir = (*helpdir ? helpdir : "./");
    int fd;

    /* try "name-help.pd" */
    strncpy(realname, helpname, MAXPDSTRING - 10);
    realname[MAXPDSTRING - 10] = 0;
    if (strlen(realname) > 3 &&
        !strcmp(realname + strlen(realname) - 3, ".pd"))
            realname[strlen(realname) - 3] = 0;
    strcat(realname, "-help.pd");
    fd = do_open_via_path(usedir, realname, "", dirbuf, &basename,
        MAXPDSTRING, 0, STUFF->st_helppath, 1);

    if (fd < 0)
    {
        /* try "help-name.pd" */
        strcpy(realname, "help-");
        strncat(realname, helpname, MAXPDSTRING - 10);
        realname[MAXPDSTRING - 1] = 0;
        fd = do_open_via_path(usedir, realname, "", dirbuf, &basename,
            MAXPDSTRING, 0, STUFF->st_helppath, 1);
    }

    if (fd < 0)
        return NULL;
    close(fd);

    /* open the patch silently (pop with vis=0, skip loadbang) */
    t_pd *x = 0, *boundx;
    int dspstate = canvas_suspend_dsp();
    boundx = s__X.s_thing;
    s__X.s_thing = 0;
    binbuf_evalfile(gensym(basename), gensym(dirbuf));
    while ((x != s__X.s_thing) && s__X.s_thing)
    {
        x = s__X.s_thing;
        pd_vmess(x, gensym("pop"), "i", 0);  /* 0 = don't make visible */
    }
    /* deliberately skip pd_doloadbang() */
    canvas_resume_dsp(dspstate);
    s__X.s_thing = boundx;

    if (!x)
        return NULL;

    /* serialize the help patch contents */
    cJSON *help = mcp_serialize_canvas((t_glist *)x);

    /* close and free the silently-opened patch */
    pd_free(x);

    return help;
}

/* ---- tool: get_object_doc ---- */
static cJSON *mcp_tool_get_object_doc(cJSON *args)
{
    const char *name = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "name"));
    if (!name)
        return mcp_wrap_content(mcp_error_result("name is required"));

    /* find class by name in the class list */
    t_class *c;
    for (c = class_getfirst(); c; c = c->c_next)
    {
        if (!strcmp(c->c_name->s_name, name))
            break;
    }

    if (!c)
        return mcp_wrap_content(mcp_error_result("class not found"));

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", name);
    cJSON_AddBoolToObject(result, "is_internal", c->c_externdir == &s_);
    cJSON_AddBoolToObject(result, "is_signal", c->c_floatsignalin >= 0);
    cJSON_AddBoolToObject(result, "is_patchable", c->c_patchable != 0);
    cJSON_AddBoolToObject(result, "has_default_inlet", c->c_firstin != 0);

    /* standard method handlers */
    if (c->c_bangmethod)
        cJSON_AddBoolToObject(result, "accepts_bang", 1);
    if (c->c_floatmethod)
        cJSON_AddBoolToObject(result, "accepts_float", 1);
    if (c->c_symbolmethod)
        cJSON_AddBoolToObject(result, "accepts_symbol", 1);
    if (c->c_listmethod)
        cJSON_AddBoolToObject(result, "accepts_list", 1);
    if (c->c_anymethod)
        cJSON_AddBoolToObject(result, "accepts_anything", 1);

    const char *helpname = class_gethelpname(c);
    if (helpname)
        cJSON_AddStringToObject(result, "help_file", helpname);

    if (c->c_externdir && c->c_externdir != &s_)
        cJSON_AddStringToObject(result, "extern_dir",
            c->c_externdir->s_name);

    /* method list (skip internal-only methods) */
    cJSON *methods = cJSON_CreateArray();
    int i;
    for (i = 0; i < c->c_nmethod; i++)
    {
        t_methodentry *me = &c->c_methods[i];
        /* skip methods whose first arg is A_CANT (internal) */
        if (me->me_arg[0] == A_CANT)
            continue;
        cJSON *mobj = cJSON_CreateObject();
        cJSON_AddStringToObject(mobj, "name",
            me->me_name ? me->me_name->s_name : "?");

        cJSON *margs = cJSON_CreateArray();
        int j;
        for (j = 0; j < MAXPDARG && me->me_arg[j] != A_NULL; j++)
        {
            const char *astr = mcp_atomtype_string(me->me_arg[j]);
            cJSON_AddItemToArray(margs, cJSON_CreateString(astr));
        }
        cJSON_AddItemToObject(mobj, "args", margs);
        cJSON_AddItemToArray(methods, mobj);
    }
    cJSON_AddItemToObject(result, "methods", methods);

    /* help patch contents */
    if (helpname)
    {
        const char *helpdir = class_gethelpdir(c);
        cJSON *help = mcp_load_help_patch(helpname,
            helpdir ? helpdir : "");
        if (help)
            cJSON_AddItemToObject(result, "help_patch", help);
    }

    return mcp_wrap_content(result);
}

/* ---- tool: save_patch ---- */
static cJSON *mcp_tool_save_patch(cJSON *args)
{
    const char *patch_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "patch_id"));
    const char *path = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "path"));

    t_glist *canvas = mcp_find_canvas_by_id(patch_id);
    if (!canvas)
        return mcp_wrap_content(mcp_error_result("patch not found"));

    /* get root canvas */
    t_canvas *root = canvas;
    while (root->gl_owner)
        root = root->gl_owner;

    if (path && *path)
    {
        /* save-as: extract dir and filename from path */
        char dir[MAXPDSTRING], file[MAXPDSTRING];
        strncpy(dir, path, MAXPDSTRING - 1);
        dir[MAXPDSTRING - 1] = 0;

        char *last_slash = strrchr(dir, '/');
#ifdef _WIN32
        {
            char *last_backslash = strrchr(dir, '\\');
            if (last_backslash > last_slash)
                last_slash = last_backslash;
        }
#endif
        if (last_slash)
        {
            strncpy(file, last_slash + 1, MAXPDSTRING - 1);
            file[MAXPDSTRING - 1] = 0;
            *last_slash = 0;
        }
        else
        {
            strncpy(file, dir, MAXPDSTRING - 1);
            file[MAXPDSTRING - 1] = 0;
            strcpy(dir, ".");
        }

        t_atom save_args[3];
        SETSYMBOL(&save_args[0], gensym(file));
        SETSYMBOL(&save_args[1], gensym(dir));
        SETFLOAT(&save_args[2], 0);
        pd_typedmess(&root->gl_pd, gensym("savetofile"), 3, save_args);
    }
    else
    {
        /* save to current file */
        const char *name = root->gl_name ? root->gl_name->s_name : "";
        if (!*name || !strncmp(name, "Untitled", 8))
            return mcp_wrap_content(mcp_error_result(
                "patch is untitled; provide a 'path' argument"));

        t_atom save_args[3];
        SETSYMBOL(&save_args[0], root->gl_name);
        SETSYMBOL(&save_args[1], canvas_getdir(root));
        SETFLOAT(&save_args[2], 0);
        pd_typedmess(&root->gl_pd, gensym("savetofile"), 3, save_args);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", 1);
    return mcp_wrap_content(result);
}

/* ---- tool: new_patch ---- */
static cJSON *mcp_tool_new_patch(cJSON *args)
{
    (void)args;
    /* must pass a valid directory so canvas_new initializes gl_env;
       otherwise canvas_getenv() crashes on the ownerless top-level patch */
    char cwd[MAXPDSTRING];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, ".");
    glob_menunew(NULL, gensym("Untitled"), gensym(cwd));

    /* the new canvas is at the head of pd_canvaslist */
    t_glist *newcanvas = pd_this->pd_canvaslist;
    char idbuf[32];
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", 1);
    if (newcanvas)
        cJSON_AddStringToObject(result, "patch_id",
            mcp_ptr_id(newcanvas, idbuf, sizeof(idbuf)));
    return mcp_wrap_content(result);
}

/* ---- tool: get_audio_midi_settings ---- */
static cJSON *mcp_tool_get_audio_midi_settings(cJSON *args)
{
    (void)args;
    cJSON *result = cJSON_CreateObject();

    /* audio settings */
    t_audiosettings as;
    sys_get_audio_settings(&as);

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddNumberToObject(audio, "sample_rate", as.a_srate);
    cJSON_AddNumberToObject(audio, "block_size", as.a_blocksize);
    cJSON_AddNumberToObject(audio, "advance_ms", as.a_advance);
    cJSON_AddBoolToObject(audio, "callback_mode", as.a_callback);
    cJSON_AddBoolToObject(audio, "is_open", audio_isopen());

    {
        cJSON *indevs = cJSON_CreateArray();
        int i;
        for (i = 0; i < as.a_nindev; i++)
        {
            cJSON *dev = cJSON_CreateObject();
            char devname[256];
            sys_audiodevnumbertoname(0, as.a_indevvec[i],
                devname, sizeof(devname));
            cJSON_AddStringToObject(dev, "name", devname);
            cJSON_AddNumberToObject(dev, "channels",
                as.a_chindevvec[i]);
            cJSON_AddItemToArray(indevs, dev);
        }
        cJSON_AddItemToObject(audio, "input_devices", indevs);
    }

    {
        cJSON *outdevs = cJSON_CreateArray();
        int i;
        for (i = 0; i < as.a_noutdev; i++)
        {
            cJSON *dev = cJSON_CreateObject();
            char devname[256];
            sys_audiodevnumbertoname(1, as.a_outdevvec[i],
                devname, sizeof(devname));
            cJSON_AddStringToObject(dev, "name", devname);
            cJSON_AddNumberToObject(dev, "channels",
                as.a_choutdevvec[i]);
            cJSON_AddItemToArray(outdevs, dev);
        }
        cJSON_AddItemToObject(audio, "output_devices", outdevs);
    }
    cJSON_AddItemToObject(result, "audio", audio);

    /* MIDI settings */
    cJSON *midi = cJSON_CreateObject();
    {
        int nmidiin, midiindev[MAXMIDIINDEV];
        int nmidiout, midioutdev[MAXMIDIOUTDEV];
        sys_get_midi_params(&nmidiin, midiindev, &nmidiout, midioutdev);

        cJSON *midi_in = cJSON_CreateArray();
        int i;
        for (i = 0; i < nmidiin; i++)
        {
            char devname[256];
            sys_mididevnumbertoname(0, midiindev[i],
                devname, sizeof(devname));
            cJSON_AddItemToArray(midi_in, cJSON_CreateString(devname));
        }
        cJSON_AddItemToObject(midi, "input_devices", midi_in);

        cJSON *midi_out = cJSON_CreateArray();
        for (i = 0; i < nmidiout; i++)
        {
            char devname[256];
            sys_mididevnumbertoname(1, midioutdev[i],
                devname, sizeof(devname));
            cJSON_AddItemToArray(midi_out, cJSON_CreateString(devname));
        }
        cJSON_AddItemToObject(midi, "output_devices", midi_out);
    }
    cJSON_AddItemToObject(result, "midi", midi);

    return mcp_wrap_content(result);
}

/* ---- tool: open_patch ---- */
static cJSON *mcp_tool_open_patch(cJSON *args)
{
    const char *path = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "path"));
    if (!path)
        return mcp_wrap_content(mcp_error_result("path is required"));

    /* split path into directory and filename */
    char dir[MAXPDSTRING], file[MAXPDSTRING];
    strncpy(dir, path, MAXPDSTRING - 1);
    dir[MAXPDSTRING - 1] = 0;

    char *last_slash = strrchr(dir, '/');
#ifdef _WIN32
    {
        char *last_backslash = strrchr(dir, '\\');
        if (last_backslash > last_slash)
            last_slash = last_backslash;
    }
#endif
    if (last_slash)
    {
        strncpy(file, last_slash + 1, MAXPDSTRING - 1);
        file[MAXPDSTRING - 1] = 0;
        *last_slash = 0;
    }
    else
    {
        strncpy(file, dir, MAXPDSTRING - 1);
        file[MAXPDSTRING - 1] = 0;
        strcpy(dir, ".");
    }

    /* check if this file is already open — return existing patch */
    {
        t_glist *gl;
        char idbuf[32];
        t_symbol *file_sym = gensym(file);
        t_symbol *dir_sym = gensym(dir);
        for (gl = pd_this->pd_canvaslist; gl; gl = gl->gl_next)
        {
            if (gl->gl_name == file_sym &&
                canvas_getdir(gl) == dir_sym)
            {
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "success", 1);
                cJSON_AddStringToObject(result, "patch_id",
                    mcp_ptr_id(gl, idbuf, sizeof(idbuf)));
                cJSON_AddBoolToObject(result, "already_open", 1);
                return mcp_wrap_content(result);
            }
        }
    }

    glob_open(NULL, gensym(file), gensym(dir), 0);

    /* newly opened patch lands at the head of the canvas list */
    {
        t_glist *gl = pd_this->pd_canvaslist;
        if (gl)
        {
            char idbuf[32];
            cJSON *result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", 1);
            cJSON_AddStringToObject(result, "patch_id",
                mcp_ptr_id(gl, idbuf, sizeof(idbuf)));
            return mcp_wrap_content(result);
        }
    }

    return mcp_wrap_content(mcp_ok_result());
}

/* ---- tool: get_pd_log ---- */
static cJSON *mcp_tool_get_pd_log(cJSON *args)
{
    cJSON *jn = args ? cJSON_GetObjectItem(args, "n") : NULL;
    cJSON *jk = args ? cJSON_GetObjectItem(args, "k") : NULL;

    int n_lines = -1;  /* -1 = entire log */
    int max_level = 4; /* 4 = all levels */

    if (jn && cJSON_IsNumber(jn) && !cJSON_IsNull(jn))
    {
        int v = (int)cJSON_GetNumberValue(jn);
        if (v > 0) n_lines = v;
    }
    if (jk && cJSON_IsString(jk))
    {
        const char *s = cJSON_GetStringValue(jk);
        if (!strcmp(s, "fatal")) max_level = 0;
        else if (!strcmp(s, "error")) max_level = 1;
        else if (!strcmp(s, "normal")) max_level = 2;
        else if (!strcmp(s, "debug")) max_level = 3;
        else if (!strcmp(s, "all")) max_level = 4;
        /* omit or unknown -> 4 (all) */
    }

    /* max_level 4 = all (no filter); 0-3 = include level <= max_level */
    int total = 0;
    int i;
    int start = (mcp_log_head - mcp_log_count + MCP_LOG_MAX_LINES) %
        MCP_LOG_MAX_LINES;

    for (i = 0; i < mcp_log_count; i++)
    {
        int idx = (start + i) % MCP_LOG_MAX_LINES;
        if (max_level >= 4 || mcp_log_ring[idx].level <= max_level)
            total++;
    }

    int skip = (n_lines > 0 && total > n_lines) ? total - n_lines : 0;
    int out_lines = (n_lines > 0 && total > n_lines) ? n_lines : total;

    /* compute output size */
    size_t out_size = 1;
    int counted = 0;
    for (i = 0; i < mcp_log_count && counted < out_lines; i++)
    {
        int idx = (start + i) % MCP_LOG_MAX_LINES;
        if (max_level >= 4 || mcp_log_ring[idx].level <= max_level)
        {
            if (skip > 0) { skip--; continue; }
            out_size += strlen(mcp_log_ring[idx].line) + 1;
            counted++;
        }
    }

    char *out = (char *)getbytes(out_size);
    if (!out)
        return mcp_wrap_content(mcp_error_result("out of memory"));
    out[0] = 0;
    skip = (n_lines > 0 && total > n_lines) ? total - n_lines : 0;
    for (i = 0; i < mcp_log_count; i++)
    {
        int idx = (start + i) % MCP_LOG_MAX_LINES;
        if (max_level >= 4 || mcp_log_ring[idx].level <= max_level)
        {
            if (skip > 0) { skip--; continue; }
            strcat(out, mcp_log_ring[idx].line);
            strcat(out, "\n");
        }
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "log", out);
    freebytes(out, out_size);
    return mcp_wrap_content(result);
}

/* ---- tool: get_audio_rms ---- */
static cJSON *mcp_tool_get_audio_rms(cJSON *args)
{
    cJSON *j_dur = cJSON_GetObjectItem(args, "duration");
    if (!j_dur || !cJSON_IsNumber(j_dur))
        return mcp_wrap_content(
            mcp_error_result("duration is required (number, in seconds)"));

    float duration = (float)cJSON_GetNumberValue(j_dur);
    if (duration <= 0.0f || duration > 120.0f)
        return mcp_wrap_content(
            mcp_error_result("duration must be between 0 and 120 seconds"));

    t_mcp_audio_rms_stats stats;
    memset(&stats, 0, sizeof(stats));

    if (mcp_audio_query_rms(duration, &stats) != 0)
        return mcp_wrap_content(
            mcp_error_result("no audio data available (is DSP running?)"));

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "dsp_running",
        canvas_dspstate != 0);
    cJSON_AddNumberToObject(result, "sample_rate", stats.sample_rate);
    cJSON_AddNumberToObject(result, "block_size", stats.block_size);
    cJSON_AddNumberToObject(result, "channels", stats.channels);
    cJSON_AddNumberToObject(result, "duration", stats.actual_duration);
    cJSON_AddNumberToObject(result, "blocks_analyzed", stats.blocks_used);

    /* independent per-channel statistics */
    cJSON *ch_arr = cJSON_CreateArray();
    int ch;
    for (ch = 0; ch < stats.channels; ch++)
    {
        cJSON *chobj = cJSON_CreateObject();
        cJSON_AddNumberToObject(chobj, "channel", ch);
        cJSON_AddNumberToObject(chobj, "mean_rms", stats.mean_rms[ch]);
        cJSON_AddNumberToObject(chobj, "var_rms", stats.var_rms[ch]);
        cJSON_AddNumberToObject(chobj, "min_rms", stats.min_rms[ch]);
        cJSON_AddNumberToObject(chobj, "max_rms", stats.max_rms[ch]);
        /* dBFS for convenience */
        float db = stats.mean_rms[ch] > 0.0f
            ? 20.0f * log10f(stats.mean_rms[ch]) : -120.0f;
        cJSON_AddNumberToObject(chobj, "mean_rms_db", db);
        cJSON_AddItemToArray(ch_arr, chobj);
    }
    cJSON_AddItemToObject(result, "channel_stats", ch_arr);

    return mcp_wrap_content(result);
}

/* ==== TOOL DISPATCH ==== */
static cJSON *mcp_dispatch_tool(const char *name, cJSON *args)
{
    logpost(0, PD_DEBUG, "mcp: tool call: %s", name);
    if (!strcmp(name, "list_patches"))
        return mcp_tool_list_patches(args);
    if (!strcmp(name, "get_patch_state"))
        return mcp_tool_get_patch_state(args);
    if (!strcmp(name, "create_object"))
        return mcp_tool_create_object(args);
    if (!strcmp(name, "delete_object"))
        return mcp_tool_delete_object(args);
    if (!strcmp(name, "modify_object"))
        return mcp_tool_modify_object(args);
    if (!strcmp(name, "move_object"))
        return mcp_tool_move_object(args);
    if (!strcmp(name, "connect"))
        return mcp_tool_connect(args);
    if (!strcmp(name, "disconnect"))
        return mcp_tool_disconnect(args);
    if (!strcmp(name, "batch_update"))
        return mcp_tool_batch_update(args);
    if (!strcmp(name, "clear_patch"))
        return mcp_tool_clear_patch(args);
    if (!strcmp(name, "send_message"))
        return mcp_tool_send_message(args);
    if (!strcmp(name, "send_bang"))
        return mcp_tool_send_bang(args);
    if (!strcmp(name, "set_number"))
        return mcp_tool_set_number(args);
    if (!strcmp(name, "set_dsp"))
        return mcp_tool_set_dsp(args);
    if (!strcmp(name, "get_dsp_state"))
        return mcp_tool_get_dsp_state(args);
    if (!strcmp(name, "get_selection"))
        return mcp_tool_get_selection(args);
    if (!strcmp(name, "list_object_names"))
        return mcp_tool_list_object_names(args);
    if (!strcmp(name, "get_object_doc"))
        return mcp_tool_get_object_doc(args);
    if (!strcmp(name, "open_patch"))
        return mcp_tool_open_patch(args);
    if (!strcmp(name, "get_pd_log"))
        return mcp_tool_get_pd_log(args);
    if (!strcmp(name, "save_patch"))
        return mcp_tool_save_patch(args);
    if (!strcmp(name, "new_patch"))
        return mcp_tool_new_patch(args);
    if (!strcmp(name, "get_audio_midi_settings"))
        return mcp_tool_get_audio_midi_settings(args);
    if (!strcmp(name, "get_audio_rms"))
        return mcp_tool_get_audio_rms(args);

    return mcp_wrap_content(mcp_error_result("unknown tool"));
}

/* ==== JSON-RPC PROTOCOL LAYER ==== */

static cJSON *mcp_handle_initialize(cJSON *id, cJSON *params)
{
    (void)params;
    mcp_generate_session_id(mcp_server.session_id, MCP_SESSION_ID_LEN);

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
    {
        char version[64];
        snprintf(version, sizeof(version), "%d.%d.%d",
            PD_MAJOR_VERSION, PD_MINOR_VERSION, PD_BUGFIX_VERSION);
        cJSON_AddStringToObject(info, "version", version);
    }
    cJSON_AddItemToObject(result, "serverInfo", info);

    cJSON_AddStringToObject(result, "instructions", MCP_INSTRUCTIONS);

    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON *mcp_handle_ping(cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(resp, "result", cJSON_CreateObject());
    return resp;
}

static cJSON *mcp_handle_tools_list(cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", mcp_build_tools_list());
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON *mcp_handle_tools_call(cJSON *id, cJSON *params)
{
    const char *name = cJSON_GetStringValue(
        cJSON_GetObjectItem(params, "name"));
    cJSON *arguments = cJSON_GetObjectItem(params, "arguments");

    if (!name)
    {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
        cJSON *err = cJSON_CreateObject();
        cJSON_AddNumberToObject(err, "code", -32602);
        cJSON_AddStringToObject(err, "message",
            "missing 'name' in params");
        cJSON_AddItemToObject(resp, "error", err);
        return resp;
    }

    cJSON *result = mcp_dispatch_tool(name, arguments);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON *mcp_jsonrpc_error(cJSON *id, int code, const char *message)
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

/* process a single JSON-RPC request, return response JSON string
   (caller must free). Returns NULL for notifications. */
static char *mcp_process_jsonrpc(const char *json_body,
    int *out_is_initialize)
{
    *out_is_initialize = 0;
    cJSON *req = cJSON_Parse(json_body);
    if (!req)
    {
        cJSON *resp = mcp_jsonrpc_error(NULL, -32700, "parse error");
        char *s = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        return s;
    }

    const char *method = cJSON_GetStringValue(
        cJSON_GetObjectItem(req, "method"));
    cJSON *id = cJSON_GetObjectItem(req, "id");
    cJSON *params = cJSON_GetObjectItem(req, "params");

    if (!method)
    {
        cJSON *resp = mcp_jsonrpc_error(id, -32600,
            "invalid request: missing method");
        char *s = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        cJSON_Delete(req);
        return s;
    }

    /* notifications (no id) — accept silently */
    int is_notification = (id == NULL);

    cJSON *resp = NULL;

    if (!strcmp(method, "initialize"))
    {
        *out_is_initialize = 1;
        resp = mcp_handle_initialize(id, params);
    }
    else if (!strcmp(method, "notifications/initialized") ||
             !strcmp(method, "notifications/cancelled"))
    {
        /* notifications: no response needed */
        cJSON_Delete(req);
        return NULL;
    }
    else if (!strcmp(method, "ping"))
    {
        resp = mcp_handle_ping(id);
    }
    else if (!strcmp(method, "tools/list"))
    {
        resp = mcp_handle_tools_list(id);
    }
    else if (!strcmp(method, "tools/call"))
    {
        resp = mcp_handle_tools_call(id, params);
    }
    else
    {
        if (is_notification)
        {
            cJSON_Delete(req);
            return NULL;
        }
        resp = mcp_jsonrpc_error(id, -32601, "method not found");
    }

    char *result = NULL;
    if (resp)
    {
        result = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
    }
    cJSON_Delete(req);
    return result;
}

/* ==== HTTP LAYER ==== */

/* parse HTTP request: extract method, path, headers, body.
   Returns 1 if complete request received, 0 if need more data. */
static int mcp_parse_http(const char *buf, int len,
    char *method_out, int method_size,
    char *path_out, int path_size,
    int *content_length_out,
    char **body_out,
    char *session_id_out, int session_id_size)
{
    /* find end of headers */
    const char *header_end = NULL;
    int i;
    for (i = 0; i < len - 3; i++)
    {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
        {
            header_end = buf + i + 4;
            break;
        }
    }
    if (!header_end) return 0;

    /* parse request line */
    const char *line_end = strstr(buf, "\r\n");
    if (!line_end) return 0;

    /* method */
    const char *p = buf;
    const char *space = memchr(p, ' ', line_end - p);
    if (!space) return 0;
    int mlen = (int)(space - p);
    if (mlen >= method_size) mlen = method_size - 1;
    memcpy(method_out, p, mlen);
    method_out[mlen] = 0;

    /* path */
    p = space + 1;
    space = memchr(p, ' ', line_end - p);
    if (!space) return 0;
    int plen = (int)(space - p);
    if (plen >= path_size) plen = path_size - 1;
    memcpy(path_out, p, plen);
    path_out[plen] = 0;

    /* parse headers for Content-Length and Mcp-Session-Id */
    *content_length_out = 0;
    session_id_out[0] = 0;
    const char *h = line_end + 2;
    while (h < header_end - 2)
    {
        const char *hend = strstr(h, "\r\n");
        if (!hend) break;
        if (!strncasecmp(h, "Content-Length:", 15))
        {
            *content_length_out = atoi(h + 15);
        }
        else if (!strncasecmp(h, "Mcp-Session-Id:", 15))
        {
            const char *val = h + 15;
            while (*val == ' ') val++;
            int slen = (int)(hend - val);
            if (slen >= session_id_size) slen = session_id_size - 1;
            memcpy(session_id_out, val, slen);
            session_id_out[slen] = 0;
        }
        h = hend + 2;
    }

    /* check if we have the full body */
    int header_size = (int)(header_end - buf);
    if (len < header_size + *content_length_out)
        return 0;

    *body_out = (char *)header_end;
    return 1;
}

/* send HTTP response */
static void mcp_send_http_response(int fd, int status_code,
    const char *status_text, const char *body, int body_len,
    const char *session_id)
{
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: POST, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Mcp-Session-Id\r\n"
        "Access-Control-Expose-Headers: Mcp-Session-Id\r\n"
        "%s%s%s"
        "\r\n",
        status_code, status_text,
        body_len,
        session_id ? "Mcp-Session-Id: " : "",
        session_id ? session_id : "",
        session_id ? "\r\n" : "");

    /* write header */
    (void)send(fd, header, hlen, 0);
    /* write body */
    if (body && body_len > 0)
        (void)send(fd, body, body_len, 0);
}

/* send a simple text response (for OPTIONS, errors) */
static void mcp_send_simple_response(int fd, int status_code,
    const char *status_text, const char *body)
{
    int body_len = body ? (int)strlen(body) : 0;
    mcp_send_http_response(fd, status_code, status_text, body, body_len,
        mcp_server.session_id[0] ? mcp_server.session_id : NULL);
}

/* ---- client poll callback ---- */
static void mcp_client_poll(void *ptr, int fd)
{
    t_mcp_client *client = (t_mcp_client *)ptr;

    /* ensure buffer exists */
    if (!client->recv_buf)
    {
        client->recv_alloc = MCP_BUF_INITIAL;
        client->recv_buf = (char *)getbytes(client->recv_alloc);
        client->recv_len = 0;
    }

    /* grow buffer if needed */
    if (client->recv_len >= client->recv_alloc - 1)
    {
        if (client->recv_alloc >= MCP_MAX_REQUEST_SIZE)
        {
            mcp_send_simple_response(fd, 413,
                "Request Entity Too Large", NULL);
            goto close_client;
        }
        int newsize = client->recv_alloc * 2;
        char *newbuf = (char *)getbytes(newsize);
        memcpy(newbuf, client->recv_buf, client->recv_len);
        freebytes(client->recv_buf, client->recv_alloc);
        client->recv_buf = newbuf;
        client->recv_alloc = newsize;
    }

    /* read data */
    int n = (int)recv(fd, client->recv_buf + client->recv_len,
        client->recv_alloc - client->recv_len - 1, 0);
    if (n <= 0)
        goto close_client;

    client->recv_len += n;
    client->recv_buf[client->recv_len] = 0;

    /* try to parse HTTP request */
    char method[16], path[256], session_id[128];
    int content_length;
    char *body;

    if (!mcp_parse_http(client->recv_buf, client->recv_len,
        method, sizeof(method), path, sizeof(path),
        &content_length, &body, session_id, sizeof(session_id)))
        return; /* need more data */

    /* handle OPTIONS (CORS preflight) */
    if (!strcmp(method, "OPTIONS"))
    {
        mcp_send_simple_response(fd, 204, "No Content", NULL);
        goto reset_client;
    }

    /* check path */
    if (strcmp(path, "/mcp") != 0)
    {
        mcp_send_simple_response(fd, 404, "Not Found",
            "{\"error\":\"not found\"}");
        goto reset_client;
    }

    /* handle DELETE (end session) */
    if (!strcmp(method, "DELETE"))
    {
        mcp_server.session_id[0] = 0;
        mcp_send_simple_response(fd, 200, "OK",
            "{\"success\":true}");
        goto close_client;
    }

    /* handle POST */
    if (strcmp(method, "POST") != 0)
    {
        mcp_send_simple_response(fd, 405, "Method Not Allowed",
            "{\"error\":\"method not allowed\"}");
        goto reset_client;
    }

    /* null-terminate the body */
    {
        char saved = body[content_length];
        body[content_length] = 0;

        int is_initialize = 0;
        char *response_json = mcp_process_jsonrpc(body, &is_initialize);
        body[content_length] = saved;

        if (response_json)
        {
            int rlen = (int)strlen(response_json);
            mcp_send_http_response(fd,
                is_initialize ? 200 : 200, "OK",
                response_json, rlen,
                mcp_server.session_id[0] ?
                    mcp_server.session_id : NULL);
            free(response_json);
        }
        else
        {
            /* notification: return 202 Accepted */
            mcp_send_http_response(fd, 202, "Accepted", "{}", 2,
                mcp_server.session_id[0] ?
                    mcp_server.session_id : NULL);
        }
    }

reset_client:
    client->recv_len = 0;
    return;

close_client:
    sys_rmpollfn(fd);
    close(fd);
    if (client->recv_buf)
    {
        freebytes(client->recv_buf, client->recv_alloc);
        client->recv_buf = NULL;
    }
    client->recv_len = 0;
    client->recv_alloc = 0;
    client->active = 0;
    client->fd = -1;
}

/* ---- listen socket poll callback ---- */
static void mcp_connect_poll(void *ptr, int fd)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int client_fd = (int)accept(fd, (struct sockaddr *)&addr, &addrlen);
    if (client_fd < 0) return;

    /* check localhost restriction */
    if (mcp_server.localhost_only)
    {
        uint32_t remote = ntohl(addr.sin_addr.s_addr);
        if (remote != INADDR_LOOPBACK)
        {
            close(client_fd);
            return;
        }
    }

    /* find a free client slot */
    int i;
    for (i = 0; i < MCP_MAX_CLIENTS; i++)
    {
        if (!mcp_server.clients[i].active)
        {
            mcp_server.clients[i].fd = client_fd;
            mcp_server.clients[i].active = 1;
            mcp_server.clients[i].recv_buf = NULL;
            mcp_server.clients[i].recv_len = 0;
            mcp_server.clients[i].recv_alloc = 0;
            sys_addpollfn(client_fd, mcp_client_poll,
                &mcp_server.clients[i]);
            return;
        }
    }

    /* no free slots */
    close(client_fd);
}

/* ==== PUBLIC API ==== */

int mcp_is_running(void)
{
    return mcp_server.running;
}

void mcp_init(void)
{
    mcp_audio_init();
}

void mcp_start(int port, int localhost_only)
{
    /* ensure audio capture is initialized (idempotent) */
    mcp_audio_init();

    if (mcp_server.running)
    {
        if (port == mcp_server.port &&
            localhost_only == mcp_server.localhost_only)
            return;
        mcp_stop();
    }

    mcp_server.port = port;
    mcp_server.localhost_only = localhost_only;

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        post("mcp: failed to create socket: %s", strerror(errno));
        return;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
        (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(mcp_server.port);
    addr.sin_addr.s_addr = mcp_server.localhost_only ?
        htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        post("mcp: failed to bind to port %d: %s",
            mcp_server.port, strerror(errno));
        close(fd);
        return;
    }

    if (listen(fd, 5) < 0)
    {
        post("mcp: failed to listen: %s", strerror(errno));
        close(fd);
        return;
    }

    mcp_server.listen_fd = fd;
    mcp_server.running = 1;

    sys_addpollfn(fd, mcp_connect_poll, NULL);
    sys_logbuffer_hook = mcp_logbuffer_append;

    post("mcp: server started on %s:%d",
        mcp_server.localhost_only ? "127.0.0.1" : "0.0.0.0",
        mcp_server.port);
    pdgui_vmess("pdtk_pd_mcp", "s", "ON");
}

void mcp_stop(void)
{
    if (!mcp_server.running) return;

    /* close all client connections */
    int i;
    for (i = 0; i < MCP_MAX_CLIENTS; i++)
    {
        if (mcp_server.clients[i].active)
        {
            sys_rmpollfn(mcp_server.clients[i].fd);
            close(mcp_server.clients[i].fd);
            if (mcp_server.clients[i].recv_buf)
                freebytes(mcp_server.clients[i].recv_buf,
                    mcp_server.clients[i].recv_alloc);
            mcp_server.clients[i].recv_buf = NULL;
            mcp_server.clients[i].active = 0;
            mcp_server.clients[i].fd = -1;
        }
    }

    /* close listening socket */
    if (mcp_server.listen_fd >= 0)
    {
        sys_rmpollfn(mcp_server.listen_fd);
        close(mcp_server.listen_fd);
        mcp_server.listen_fd = -1;
    }

    mcp_server.running = 0;
    mcp_server.session_id[0] = 0;
    sys_logbuffer_hook = NULL;

    post("mcp: server stopped");
    pdgui_vmess("pdtk_pd_mcp", "s", "OFF");
}

void mcp_free(void)
{
    mcp_stop();
    mcp_audio_free();
}

/* ---- glob methods (called from Tcl via pdsend) ---- */

void glob_mcp(void *dummy, t_symbol *s, int argc, t_atom *argv)
{
    (void)dummy;
    (void)s;
    if (argc)
    {
        int state = (int)atom_getfloat(argv);
        if (state)
            mcp_start(mcp_server.port, mcp_server.localhost_only);
        else
            mcp_stop();
        mcp_server.enabled = state;
    }
    else
        post("mcp: server %s (port %d, %s)",
            mcp_server.running ? "running" : "stopped",
            mcp_server.port,
            mcp_server.localhost_only ? "localhost only" : "network");
}

void glob_mcp_port(void *dummy, t_floatarg port)
{
    (void)dummy;
    int p = (int)port;
    if (p < 1 || p > 65535)
    {
        post("mcp: invalid port number %d", p);
        return;
    }
    int was_running = mcp_server.running;
    if (was_running) mcp_stop();
    mcp_server.port = p;
    if (was_running) mcp_start(mcp_server.port, mcp_server.localhost_only);
    post("mcp: port set to %d", p);
}

void glob_mcp_network(void *dummy, t_floatarg allow)
{
    (void)dummy;
    int was_running = mcp_server.running;
    if (was_running) mcp_stop();
    mcp_server.localhost_only = !(int)allow;
    if (was_running) mcp_start(mcp_server.port, mcp_server.localhost_only);
    post("mcp: network access %s",
        mcp_server.localhost_only ? "disabled" : "enabled");
}
