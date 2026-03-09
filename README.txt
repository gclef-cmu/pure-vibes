# Pure Vibes (Pd-vibes)

**Pure Vibes is an unofficial, experimental fork of [Pure Data](https://puredata.info) (Pd 0.56.2) with a native MCP server built in.** It is vibe-coded and intended for experimental use only. It is not affiliated with or endorsed by Miller Puckette or the Pure Data community.

Pure Vibes lets AI agents (Claude, ChatGPT, etc.) read, create, and manipulate Pd patches in real time through the [Model Context Protocol](https://modelcontextprotocol.io). You install it, check the "MCP" box, and point your AI app at it. No Python, no Node, no bridge patches.

---

## Quick Start (for musicians)

### 1. Install Pd-vibes

Download the latest release for your platform from the **[Releases](../../releases)** tab:

- **macOS**: Download `Pd-vibes-macos.zip`, unzip, drag `Pd-vibes.app` to Applications
- **Windows**: Download `Pd-vibes-windows-x86_64.tar.gz`, extract, run `pd.exe`
- **Linux**: Download `Pd-vibes-linux-x86_64.tar.gz`, extract, run `bin/pd`

Launch Pd-vibes. You will see an "MCP" checkbox in the main window (next to DSP). It is enabled by default, listening on port 4330.

### 2. Connect to Claude Desktop

Add this to your Claude Desktop config file:

**macOS**: `~/Library/Application Support/Claude/claude_desktop_config.json`
**Windows**: `%APPDATA%\Claude\claude_desktop_config.json`

```json
{
  "mcpServers": {
    "pure-vibes": {
      "url": "http://localhost:4330/mcp"
    }
  }
}
```

Restart Claude Desktop. Pure Vibes should appear as a connected MCP server.

### 3. Try it out

Open a new patch in Pd-vibes (Cmd+N / Ctrl+N), then ask Claude:

> "Build me a simple synthesizer in Pure Data with an oscillator, envelope, and volume control"

Claude will use the MCP tools to create objects, wire them together, and you will see the patch build itself in real time.

Other things to try:

> "What patches do I have open?"

> "Turn on DSP"

> "Add a reverb to my patch"

### 4. Connect to ChatGPT Desktop (or other MCP clients)

Any MCP client that supports the Streamable HTTP transport can connect. Point it at:

    http://localhost:4330/mcp

---

## MCP Configuration

- **Toggle**: Check/uncheck "MCP" in the main Pd window, or use the Media menu
- **Port**: Media > MCP Port... (default: 4330). CLI: `-mcpport 4331`
- **Network**: Media > MCP Allow Network (default: localhost only). CLI: `-mcpnetwork`
- **Disable**: Uncheck "MCP" or start with `-nomcp`

---

## Building from Source

### macOS (arm64 / x86_64)

Install Xcode command line tools and Homebrew, then:

```sh
brew install autoconf automake libtool gettext
LIBTOOLIZE=$(brew --prefix libtool)/bin/glibtoolize ./autogen.sh
./configure
make -j$(sysctl -n hw.logicalcpu)
```

To create an app bundle:

```sh
make app
# Creates Pd-vibes-0.56.2.app in the build directory
```

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install autoconf automake libtool gettext \
    libasound2-dev libjack-jackd2-dev tcl-dev tk-dev
./autogen.sh
./configure
make -j$(nproc)
sudo make install
```

### Windows (MSYS2)

Install [MSYS2](https://www.msys2.org), open a MINGW64 shell, then:

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-autotools make autoconf automake libtool
./autogen.sh
./configure
make -j$(nproc)
```

For more detailed build instructions, see INSTALL.txt.

---

## What the MCP Server Can Do

The built-in MCP server exposes 19 tools:

| Category | Tools |
|----------|-------|
| Patches | `list_patches`, `get_patch_state`, `open_patch` |
| Objects | `create_object`, `delete_object`, `modify_object`, `move_object` |
| Connections | `connect`, `disconnect` |
| Batch | `batch_update`, `clear_patch` |
| Runtime | `send_message`, `send_bang`, `set_number` |
| DSP | `set_dsp`, `get_dsp_state` |
| Selection | `get_selection` |
| Docs | `list_externals`, `get_object_doc` |

---

## About Pure Data

Pure Data (Pd) is a free, open-source visual programming language for multimedia,
created by Miller Puckette. For information about vanilla Pd, visit:

- https://puredata.info
- http://msp.ucsd.edu/software.html

---

## Copyright & Licensing

### Pure Data

Except as otherwise noted, all files in the Pd distribution are:

    Copyright (c) 1997-2024 Miller Puckette and others.

Licensed under the **BSD 3-Clause License**. See LICENSE.txt for details.

### cJSON (embedded JSON library)

The files `src/s_mcp_cjson.c` and `src/s_mcp_cjson.h` are from the
[cJSON](https://github.com/DaveGamble/cJSON) project (v1.7.18):

    Copyright (c) 2009-2017 Dave Gamble and cJSON contributors.

Licensed under the **MIT License**. The full license text is included at the
top of each file.

### MCP Server Code

The files `src/s_mcp.c` and `src/s_mcp.h` are new additions to this fork,
written for the Pure Vibes project. They are released under the same
**BSD 3-Clause License** as the rest of Pure Data.

### Compatibility

The BSD 3-Clause (Pd) and MIT (cJSON) licenses are fully compatible.
Both are permissive open-source licenses that allow free use, modification,
and redistribution.
