/* mcp_tools.c - Shared MCP tool definitions
 *
 * Contains the JSON schema builders for all MCP tools. Used by both
 * the server (mcp_server.c) and the proxy (mcp_proxy.c). Depends only
 * on cJSON — no Pd internals.
 */

#include "mcp_tools.h"

/* ---- JSON schema helpers ---- */

cJSON *mcp_prop_string(const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", desc);
    return p;
}

cJSON *mcp_prop_int(const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "integer");
    cJSON_AddStringToObject(p, "description", desc);
    return p;
}

cJSON *mcp_prop_number(const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "number");
    cJSON_AddStringToObject(p, "description", desc);
    return p;
}

cJSON *mcp_prop_bool(const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "boolean");
    cJSON_AddStringToObject(p, "description", desc);
    return p;
}

cJSON *mcp_make_schema(const char *required[], int nrequired)
{
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
    if (nrequired > 0)
    {
        cJSON *req = cJSON_CreateArray();
        int i;
        for (i = 0; i < nrequired; i++)
            cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
        cJSON_AddItemToObject(schema, "required", req);
    }
    return schema;
}

void mcp_schema_add_prop(cJSON *schema, const char *name, cJSON *prop)
{
    cJSON_AddItemToObject(cJSON_GetObjectItem(schema, "properties"), name,
        prop);
}

/* ---- tool list ---- */

cJSON *mcp_build_tools_list(void)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool, *schema;

    /* list_patches */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "list_patches");
    cJSON_AddStringToObject(tool, "description",
        "List all currently open patches with their names, file paths, "
        "dirty flags, and dimensions");
    schema = mcp_make_schema(NULL, 0);
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* get_patch_state */
    {
        const char *req[] = {"patch_id"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "get_patch_state");
        cJSON_AddStringToObject(tool, "description",
            "Get the full state of a patch: all objects (id, class, text, "
            "position, type, inlet/outlet counts) and all connections. "
            "Objects with is_subpatch=true can be passed as patch_id to "
            "inspect their contents.");
        schema = mcp_make_schema(req, 1);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch or subpatch ID from list_patches or "
                "get_patch_state"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* create_object */
    {
        const char *req[] = {"patch_id", "type", "x", "y"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "create_object");
        cJSON_AddStringToObject(tool, "description",
            "Create a single object in a patch. Type can be 'obj', 'msg', "
            "'text', 'floatatom', or 'symbolatom'. For 'obj', text is "
            "the object name and arguments (e.g. 'osc~ 440'). "
            "NOTE: When creating multiple objects or objects that need "
            "connections, use batch_update instead for atomicity and "
            "fewer round trips.");
        schema = mcp_make_schema(req, 4);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "type",
            mcp_prop_string("Box type: obj, msg, text, floatatom, "
                "symbolatom"));
        mcp_schema_add_prop(schema, "text",
            mcp_prop_string("Object text (e.g. 'osc~ 440')"));
        mcp_schema_add_prop(schema, "x",
            mcp_prop_int("X position in pixels"));
        mcp_schema_add_prop(schema, "y",
            mcp_prop_int("Y position in pixels"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* delete_object */
    {
        const char *req[] = {"patch_id", "object_id"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "delete_object");
        cJSON_AddStringToObject(tool, "description",
            "Delete a single object from a patch. "
            "Prefer batch_update when deleting multiple objects.");
        schema = mcp_make_schema(req, 2);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "object_id",
            mcp_prop_string("Object ID from get_patch_state"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* modify_object */
    {
        const char *req[] = {"patch_id", "object_id", "text"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "modify_object");
        cJSON_AddStringToObject(tool, "description",
            "Change the text of a single object (will re-instantiate it). "
            "Prefer batch_update when modifying multiple objects.");
        schema = mcp_make_schema(req, 3);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "object_id",
            mcp_prop_string("Object ID"));
        mcp_schema_add_prop(schema, "text",
            mcp_prop_string("New object text"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* move_object */
    {
        const char *req[] = {"patch_id", "object_id", "x", "y"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "move_object");
        cJSON_AddStringToObject(tool, "description",
            "Move a single object to a new position. "
            "Prefer batch_update when repositioning multiple objects.");
        schema = mcp_make_schema(req, 4);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "object_id",
            mcp_prop_string("Object ID"));
        mcp_schema_add_prop(schema, "x",
            mcp_prop_int("New X position"));
        mcp_schema_add_prop(schema, "y",
            mcp_prop_int("New Y position"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* connect */
    {
        const char *req[] = {"patch_id", "src_id", "outlet", "dst_id",
            "inlet"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "connect");
        cJSON_AddStringToObject(tool, "description",
            "Connect an outlet of one object to an inlet of another. "
            "Only use for a single connection in isolation. "
            "Prefer batch_update when making multiple connections or "
            "combining with object creation.");
        schema = mcp_make_schema(req, 5);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "src_id",
            mcp_prop_string("Source object ID"));
        mcp_schema_add_prop(schema, "outlet",
            mcp_prop_int("Outlet number (0-based)"));
        mcp_schema_add_prop(schema, "dst_id",
            mcp_prop_string("Destination object ID"));
        mcp_schema_add_prop(schema, "inlet",
            mcp_prop_int("Inlet number (0-based)"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* disconnect */
    {
        const char *req[] = {"patch_id", "src_id", "outlet", "dst_id",
            "inlet"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "disconnect");
        cJSON_AddStringToObject(tool, "description",
            "Disconnect an outlet from an inlet. "
            "Prefer batch_update when making multiple disconnections.");
        schema = mcp_make_schema(req, 5);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "src_id",
            mcp_prop_string("Source object ID"));
        mcp_schema_add_prop(schema, "outlet",
            mcp_prop_int("Outlet number"));
        mcp_schema_add_prop(schema, "dst_id",
            mcp_prop_string("Destination object ID"));
        mcp_schema_add_prop(schema, "inlet",
            mcp_prop_int("Inlet number"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* batch_update */
    {
        const char *req[] = {"patch_id", "operations"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "batch_update");
        cJSON_AddStringToObject(tool, "description",
            "PREFERRED tool for patch editing. Executes multiple "
            "operations atomically in a single call with DSP suspended. "
            "Always use this instead of individual create_object/connect/"
            "delete_object/move_object/modify_object/disconnect calls "
            "when performing more than one operation. Each operation is "
            "{\"tool\": \"<tool_name>\", \"args\": {<tool_args>}}. "
            "Supports all mutation tools. Operations execute in order. "
            "To connect newly created objects, read the returned object "
            "IDs from the results and use them in a subsequent call.");
        schema = mcp_make_schema(req, 2);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        {
            cJSON *ops = cJSON_CreateObject();
            cJSON_AddStringToObject(ops, "type", "array");
            cJSON_AddStringToObject(ops, "description",
                "Array of operations. Each element is "
                "{\"tool\": \"create_object\", \"args\": {...}} etc. "
                "Supported tools: create_object, delete_object, "
                "modify_object, move_object, connect, disconnect. "
                "Executed in order within a single atomic transaction.");
            mcp_schema_add_prop(schema, "operations", ops);
        }
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* clear_patch */
    {
        const char *req[] = {"patch_id"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "clear_patch");
        cJSON_AddStringToObject(tool, "description",
            "Delete all objects in a patch");
        schema = mcp_make_schema(req, 1);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* send_message */
    {
        const char *req[] = {"patch_id", "object_id", "message"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "send_message");
        cJSON_AddStringToObject(tool, "description",
            "Send a message to an object's first inlet. The message is "
            "a string that will be parsed as Pd atoms (e.g. 'set 440' "
            "or 'bang')");
        schema = mcp_make_schema(req, 3);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "object_id",
            mcp_prop_string("Object ID"));
        mcp_schema_add_prop(schema, "message",
            mcp_prop_string("Message to send (e.g. 'bang', 'set 440')"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* send_bang */
    {
        const char *req[] = {"patch_id", "object_id"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "send_bang");
        cJSON_AddStringToObject(tool, "description",
            "Send a bang to an object's first inlet");
        schema = mcp_make_schema(req, 2);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "object_id",
            mcp_prop_string("Object ID"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* set_number */
    {
        const char *req[] = {"patch_id", "object_id", "value"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "set_number");
        cJSON_AddStringToObject(tool, "description",
            "Send a float value to an object's first inlet");
        schema = mcp_make_schema(req, 3);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "object_id",
            mcp_prop_string("Object ID"));
        mcp_schema_add_prop(schema, "value",
            mcp_prop_number("Float value to send"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* set_dsp */
    {
        const char *req[] = {"enabled"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "set_dsp");
        cJSON_AddStringToObject(tool, "description",
            "Turn DSP audio processing on or off");
        schema = mcp_make_schema(req, 1);
        mcp_schema_add_prop(schema, "enabled",
            mcp_prop_bool("true to enable DSP, false to disable"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* get_dsp_state */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "get_dsp_state");
    cJSON_AddStringToObject(tool, "description",
        "Get the current DSP (audio) state");
    schema = mcp_make_schema(NULL, 0);
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* get_selection */
    {
        const char *req[] = {"patch_id"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "get_selection");
        cJSON_AddStringToObject(tool, "description",
            "Get the currently selected objects in a patch");
        schema = mcp_make_schema(req, 1);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* list_object_names */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "list_object_names");
    cJSON_AddStringToObject(tool, "description",
        "List all available object classes, split into internals "
        "(built-in) and externals (loaded libraries)");
    schema = mcp_make_schema(NULL, 0);
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* get_object_doc */
    {
        const char *req[] = {"name"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "get_object_doc");
        cJSON_AddStringToObject(tool, "description",
            "Get documentation for a Pd object class: accepted message "
            "types, methods with argument types, signal status, and more");
        schema = mcp_make_schema(req, 1);
        mcp_schema_add_prop(schema, "name",
            mcp_prop_string("Object class name (e.g. 'osc~')"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* open_patch */
    {
        const char *req[] = {"path"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "open_patch");
        cJSON_AddStringToObject(tool, "description",
            "Open a patch file by path");
        schema = mcp_make_schema(req, 1);
        mcp_schema_add_prop(schema, "path",
            mcp_prop_string("Full file path to the .pd file"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* save_patch */
    {
        const char *req[] = {"patch_id"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "save_patch");
        cJSON_AddStringToObject(tool, "description",
            "Save a patch to disk. If the patch already has a filename, "
            "saves in place. Provide 'path' for save-as to a new location.");
        schema = mcp_make_schema(req, 1);
        mcp_schema_add_prop(schema, "patch_id",
            mcp_prop_string("Patch ID"));
        mcp_schema_add_prop(schema, "path",
            mcp_prop_string("Optional file path for save-as "
                "(e.g. '/Users/me/patches/my_patch.pd')"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* new_patch */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "new_patch");
    cJSON_AddStringToObject(tool, "description",
        "Create a new empty patch window. Returns the new patch ID. "
        "Automatically places a 'Created with Pure Vibes' text comment "
        "at (10, 10). Account for this when laying out objects — "
        "start placing your own objects below y=30 to avoid overlap.");
    schema = mcp_make_schema(NULL, 0);
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* get_audio_midi_settings */
    tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "get_audio_midi_settings");
    cJSON_AddStringToObject(tool, "description",
        "Get current audio and MIDI device settings (sample rate, "
        "block size, input/output devices and channels, MIDI devices). "
        "Read-only. To change audio or MIDI settings, ask the user to "
        "open Pd's Audio Settings or MIDI Settings dialog from the "
        "Media menu and configure manually.");
    schema = mcp_make_schema(NULL, 0);
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);

    /* get_pd_log */
    {
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "get_pd_log");
        cJSON_AddStringToObject(tool, "description",
            "Return the most recent N lines of the Pd log output. "
            "N omitted = entire log. K omitted = 'all'. "
            "K values: 'fatal', 'error', 'normal', 'debug', 'all'");
        schema = mcp_make_schema(NULL, 0);
        mcp_schema_add_prop(schema, "n",
            mcp_prop_int("Number of most recent lines; omit for entire log"));
        mcp_schema_add_prop(schema, "k",
            mcp_prop_string("Max log level: 'fatal','error','normal',"
                "'debug','all'; omit for 'all'"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    /* get_audio_rms */
    {
        const char *req[] = {"duration"};
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "get_audio_rms");
        cJSON_AddStringToObject(tool, "description",
            "Get RMS (root mean square) audio level statistics from Pd's "
            "DAC output stream over a recent time window. Only audio "
            "routed to [dac~] is captured. Capture pauses when DSP is off, "
            "preserving the last real audio data. Returns independent "
            "per-channel mean, variance, min, and max RMS plus dBFS. "
            "Response includes dsp_running flag; if false, data is from "
            "the last DSP session.");
        schema = mcp_make_schema(req, 1);
        mcp_schema_add_prop(schema, "duration",
            mcp_prop_number("Seconds of recent audio to analyze "
                "(e.g. 1.0 for last second, up to ~60 s)"));
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    return tools;
}
